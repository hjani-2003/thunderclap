#include "clap.h"

#include <stdarg.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <netdb.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

int g_log_level = LOG_INFO;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

void logmsg(int level, const char *fmt, ...)
{
    static const char *tags[] = { "debug", "info", "warn", "error" };
    if (level < g_log_level)
        return;
    char ts[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%H:%M:%S", &tm);

    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&log_lock);
    fprintf(stderr, "%s [%s] ", ts, tags[level]);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    pthread_mutex_unlock(&log_lock);
    va_end(ap);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        LOGE("out of memory");
        abort();
    }
    return p;
}

void *xcalloc(size_t n, size_t m)
{
    void *p = calloc(n ? n : 1, m ? m : 1);
    if (!p) {
        LOGE("out of memory");
        abort();
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) {
        LOGE("out of memory");
        abort();
    }
    return p;
}

char *xstrdup(const char *s)
{
    char *d = strdup(s ? s : "");
    if (!d)
        abort();
    return d;
}

double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int64_t wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * n] = 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int hex_decode(const char *in, uint8_t *out, size_t n)
{
    if (!in || strlen(in) != 2 * n)
        return -1;
    for (size_t i = 0; i < n; i++) {
        int hi = hexval(in[2 * i]), lo = hexval(in[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

int mkdir_p(const char *path, mode_t mode)
{
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp)
        return -1;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, mode) < 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

void human_bytes(double v, char *out, size_t n)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    int u = 0;
    while (v >= 1024 && u < 4) {
        v /= 1024;
        u++;
    }
    snprintf(out, n, u ? "%.1f %s" : "%.0f %s", v, units[u]);
}

void human_rate(double bps, char *out, size_t n)
{
    double bits = bps * 8;
    if (bits >= 1e9)
        snprintf(out, n, "%.2f Gbit/s", bits / 1e9);
    else if (bits >= 1e6)
        snprintf(out, n, "%.1f Mbit/s", bits / 1e6);
    else
        snprintf(out, n, "%.0f kbit/s", bits / 1e3);
}

int read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

int write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r < 0 && errno == ENOTSOCK)
            r = write(fd, p, n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

void tcp_tune(int fd)
{
    int buf = 8 << 20, one = 1;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
}

int tcp_listen(const char *bind_addr, int port)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE };
    struct addrinfo *res;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(bind_addr, ps, &hints, &res) != 0)
        return -1;
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, 0);
        if (fd < 0)
            continue;
        int one = 1, zero = 0;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (ai->ai_family == AF_INET6)
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 128) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int tcp_connect(const char *host, int port, int timeout_ms)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0)
        return -1;
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, 0);
        if (fd < 0)
            continue;
        tcp_tune(fd);
        int fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            if (poll(&pfd, 1, timeout_ms) == 1) {
                int err = 0;
                socklen_t el = sizeof err;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
                rc = err ? -1 : 0;
                errno = err;
            } else {
                errno = ETIMEDOUT;
            }
        }
        if (rc == 0) {
            fcntl(fd, F_SETFL, fl);
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Accepts "host", "host:port", "[v6]:port". */
int parse_host_port(const char *s, char *host, size_t hn, int *port, int def_port)
{
    *port = def_port;
    if (s[0] == '[') {
        const char *e = strchr(s, ']');
        if (!e || (size_t)(e - s - 1) >= hn)
            return -1;
        memcpy(host, s + 1, e - s - 1);
        host[e - s - 1] = 0;
        if (e[1] == ':')
            *port = atoi(e + 2);
        return 0;
    }
    const char *c = strrchr(s, ':');
    if (c && strchr(s, ':') == c) {
        if ((size_t)(c - s) >= hn)
            return -1;
        memcpy(host, s, c - s);
        host[c - s] = 0;
        *port = atoi(c + 1);
    } else {
        snprintf(host, hn, "%s", s);
    }
    return *port > 0 && *port < 65536 ? 0 : -1;
}

void sockaddr_str(const struct sockaddr_storage *ss, char *out, size_t n)
{
    if (ss->ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)ss)->sin_addr, out, n);
    } else if (ss->ss_family == AF_INET6) {
        const struct in6_addr *a = &((struct sockaddr_in6 *)ss)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(a))
            inet_ntop(AF_INET, &a->s6_addr[12], out, n);
        else
            inet_ntop(AF_INET6, a, out, n);
    } else {
        snprintf(out, n, "?");
    }
}

/* ---- buf_t ---- */

void buf_reserve(buf_t *b, size_t extra)
{
    if (b->len + extra <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra)
        cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void buf_put(buf_t *b, const void *p, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

void buf_u8(buf_t *b, uint8_t v) { buf_put(b, &v, 1); }
void buf_u16(buf_t *b, uint16_t v) { uint8_t t[2]; put_u16(t, v); buf_put(b, t, 2); }
void buf_u32(buf_t *b, uint32_t v) { uint8_t t[4]; put_u32(t, v); buf_put(b, t, 4); }
void buf_u64(buf_t *b, uint64_t v) { uint8_t t[8]; put_u64(t, v); buf_put(b, t, 8); }

void buf_str(buf_t *b, const char *s)
{
    size_t n = strlen(s);
    if (n > 65535)
        n = 65535;
    buf_u16(b, (uint16_t)n);
    buf_put(b, s, n);
}

void buf_free(buf_t *b)
{
    free(b->data);
    memset(b, 0, sizeof *b);
}

/* ---- rd_t ---- */

const uint8_t *rd_bytes(rd_t *r, size_t n)
{
    if (r->err || (size_t)(r->end - r->p) < n) {
        r->err = true;
        return NULL;
    }
    const uint8_t *p = r->p;
    r->p += n;
    return p;
}

uint8_t rd_u8(rd_t *r) { const uint8_t *p = rd_bytes(r, 1); return p ? p[0] : 0; }
uint16_t rd_u16(rd_t *r) { const uint8_t *p = rd_bytes(r, 2); return p ? get_u16(p) : 0; }
uint32_t rd_u32(rd_t *r) { const uint8_t *p = rd_bytes(r, 4); return p ? get_u32(p) : 0; }
uint64_t rd_u64(rd_t *r) { const uint8_t *p = rd_bytes(r, 8); return p ? get_u64(p) : 0; }

char *rd_str(rd_t *r)
{
    uint16_t n = rd_u16(r);
    const uint8_t *p = rd_bytes(r, n);
    if (!p)
        return NULL;
    char *s = xmalloc(n + 1u);
    memcpy(s, p, n);
    s[n] = 0;
    return s;
}
