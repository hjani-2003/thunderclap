/* Minimal HTTP/1.1 server for the web UI: static files built into the
 * binary, a JSON API behind a bearer token, and Server-Sent Events. */
#include "daemon.h"
#include "api.h"
#include "config.h"

#include <sys/socket.h>
#include <sys/time.h>

#define MAX_HEADER (16 * 1024)
#define MAX_BODY   (1 << 20)

/* The web UI, compiled into the binary (build/webui.c, made by tools/bin2c.c). */
extern const char index_html[], app_js[], style_css[];
extern const size_t index_html_len, app_js_len, style_css_len;

typedef struct {
    char method[8];
    char path[1024];
    char query[1024];
    char host[256];
    char origin[256];
    char auth[256];
    size_t content_length;
    char *body;
} req_t;

static void respond(int fd, int status, const char *ctype, const char *body, size_t n)
{
    const char *reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 403 ? "Forbidden"
                       : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : "Error";
    char hdr[1024];
    int hn = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %zu\r\n"
                      "Cache-Control: no-store\r\n"
                      "X-Content-Type-Options: nosniff\r\n"
                      "X-Frame-Options: DENY\r\n"
                      "Referrer-Policy: no-referrer\r\n"
                      "Content-Security-Policy: default-src 'self'; style-src 'self'; script-src 'self'; "
                      "connect-src 'self'; img-src 'self' data:\r\n"
                      "Connection: close\r\n\r\n",
                      status, reason, ctype, n);
    write_full(fd, hdr, (size_t)hn);
    if (n)
        write_full(fd, body, n);
}

static void respond_json(int fd, int status, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    respond(fd, status, "application/json", s, strlen(s));
    free(s);
}

static void respond_error(int fd, int status, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", msg);
    respond_json(fd, status, j);
    cJSON_Delete(j);
}

static void header_value(const char *line, const char *name, char *out, size_t n)
{
    size_t nl = strlen(name);
    if (strncasecmp(line, name, nl) || line[nl] != ':')
        return;
    const char *v = line + nl + 1;
    while (*v == ' ' || *v == '\t')
        v++;
    snprintf(out, n, "%s", v);
}

static int read_request(int fd, req_t *r)
{
    char *buf = xmalloc(MAX_HEADER + 1);
    size_t n = 0;
    char *end = NULL;
    while (!end && n < MAX_HEADER) {
        ssize_t got = read(fd, buf + n, MAX_HEADER - n);
        if (got <= 0)
            break;
        n += (size_t)got;
        buf[n] = 0;
        end = strstr(buf, "\r\n\r\n");
    }
    if (!end) {
        free(buf);
        return -1;
    }
    *end = 0;
    char *body_start = end + 4;
    size_t have = n - (size_t)(body_start - buf);

    char *save, *line = strtok_r(buf, "\r\n", &save);
    char target[2048];
    if (!line || sscanf(line, "%7s %2047s", r->method, target) != 2) {
        free(buf);
        return -1;
    }
    char *q = strchr(target, '?');
    if (q) {
        *q = 0;
        snprintf(r->query, sizeof r->query, "%s", q + 1);
    }
    snprintf(r->path, sizeof r->path, "%s", target);
    char clen[32] = "";
    while ((line = strtok_r(NULL, "\r\n", &save))) {
        header_value(line, "Host", r->host, sizeof r->host);
        header_value(line, "Origin", r->origin, sizeof r->origin);
        header_value(line, "Authorization", r->auth, sizeof r->auth);
        header_value(line, "Content-Length", clen, sizeof clen);
    }
    r->content_length = (size_t)strtoul(clen, NULL, 10);
    if (r->content_length > MAX_BODY) {
        free(buf);
        return -1;
    }
    r->body = xmalloc(r->content_length + 1);
    memcpy(r->body, body_start, MIN(have, r->content_length));
    if (have < r->content_length && read_full(fd, r->body + have, r->content_length - have) < 0) {
        free(buf);
        return -1;
    }
    r->body[r->content_length] = 0;
    free(buf);
    return 0;
}

/* Only accept requests addressed to our loopback origin. This defeats
 * DNS-rebinding (Host: evil.example) and cross-site requests. */
static bool host_ok(const char *host)
{
    char want[3][64];
    snprintf(want[0], sizeof want[0], "127.0.0.1:%d", g_cfg.web_port);
    snprintf(want[1], sizeof want[1], "localhost:%d", g_cfg.web_port);
    snprintf(want[2], sizeof want[2], "[::1]:%d", g_cfg.web_port);
    for (int i = 0; i < 3; i++)
        if (!strcasecmp(host, want[i]))
            return true;
    return false;
}

static bool origin_ok(const char *origin)
{
    if (!*origin)
        return true;
    const char *p = strstr(origin, "://");
    return p && host_ok(p + 3);
}

static bool token_ok(const char *tok)
{
    size_t n = strlen(g_cfg.web_token);
    return tok && strlen(tok) == n && sodium_memcmp(tok, g_cfg.web_token, n) == 0;
}

static const char *query_param(const char *query, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    for (const char *p = query; p && *p;) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t vl = strcspn(v, "&");
            snprintf(out, n, "%.*s", (int)MIN(vl, n - 1), v);
            return out;
        }
        p = strchr(p, '&');
        if (p)
            p++;
    }
    return NULL;
}

static void serve_events(int fd)
{
    const char *hdr = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/event-stream\r\n"
                      "Cache-Control: no-store\r\n"
                      "X-Accel-Buffering: no\r\n"
                      "Connection: keep-alive\r\n\r\n"
                      "retry: 1000\n\n";
    if (write_full(fd, hdr, strlen(hdr)) < 0)
        return;
    struct timeval tv = { .tv_sec = 5 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    for (;;) {
        cJSON *st = api_state();
        char *s = cJSON_PrintUnformatted(st);
        cJSON_Delete(st);
        int rc = write_full(fd, "data: ", 6) | write_full(fd, s, strlen(s)) | write_full(fd, "\n\n", 2);
        free(s);
        if (rc < 0)
            return;
        usleep(500 * 1000);
    }
}

void http_serve(int fd)
{
    struct timeval tv = { .tv_sec = 10 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    req_t r = { 0 };
    if (read_request(fd, &r) < 0) {
        respond_error(fd, 400, "bad request");
        goto out;
    }
    if (!host_ok(r.host) || !origin_ok(r.origin)) {
        respond_error(fd, 403, "forbidden host or origin");
        goto out;
    }

    if (!strcmp(r.method, "GET")) {
        if (!strcmp(r.path, "/") || !strcmp(r.path, "/index.html"))
            respond(fd, 200, "text/html; charset=utf-8", index_html, index_html_len);
        else if (!strcmp(r.path, "/app.js"))
            respond(fd, 200, "text/javascript; charset=utf-8", app_js, app_js_len);
        else if (!strcmp(r.path, "/style.css"))
            respond(fd, 200, "text/css; charset=utf-8", style_css, style_css_len);
        else if (!strcmp(r.path, "/api/events")) {
            /* EventSource cannot send headers, so the token rides in the query. */
            char tok[128];
            if (!token_ok(query_param(r.query, "token", tok, sizeof tok)))
                respond_error(fd, 403, "missing or wrong token — open the UI with `clap ui`");
            else
                serve_events(fd);
        } else
            respond_error(fd, 404, "not found");
        goto out;
    }

    if (strcmp(r.method, "POST") || strncmp(r.path, "/api/", 5)) {
        respond_error(fd, 405, "method not allowed");
        goto out;
    }
    const char *tok = !strncmp(r.auth, "Bearer ", 7) ? r.auth + 7 : NULL;
    if (!token_ok(tok)) {
        respond_error(fd, 403, "missing or wrong token — open the UI with `clap ui`");
        goto out;
    }
    cJSON *args = r.content_length ? cJSON_Parse(r.body) : cJSON_CreateObject();
    if (!args) {
        respond_error(fd, 400, "body is not JSON");
        goto out;
    }
    cJSON *resp = api_call(r.path + 5, args);
    respond_json(fd, cJSON_GetObjectItem(resp, "error") ? 400 : 200, resp);
    cJSON_Delete(resp);
    cJSON_Delete(args);
out:
    free(r.body);
    close(fd);
}
