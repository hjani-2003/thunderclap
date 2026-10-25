#include "discovery.h"
#include "config.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/stat.h>

#define MCAST_GROUP  "239.255.77.78"
#define BEACON_MAGIC "BEAM1 "
#define EXPIRE_SECS  10.0
#define MAX_DPEERS   64
#define MAX_ADDRS    4

typedef struct {
    char addr[64];
    char iface[IF_NAMESIZE];
    int speed;            /* Mbit/s; 0 = unknown */
    bool wireless;
    double last_seen;
} disc_addr_t;

typedef struct {
    char name[128];
    char fp[17];
    int port;
    disc_addr_t addrs[MAX_ADDRS];
    int n_addrs;
    double last_seen;
} dpeer_t;

static pthread_mutex_t disc_lock = PTHREAD_MUTEX_INITIALIZER;
static dpeer_t g_dpeers[MAX_DPEERS];
static int g_n_dpeers;

/* ---- interfaces ---- */

static int iface_speed(const char *ifname, bool *wireless)
{
    char path[128];
    struct stat st;
    snprintf(path, sizeof path, "/sys/class/net/%s/wireless", ifname);
    *wireless = stat(path, &st) == 0;
    snprintf(path, sizeof path, "/sys/class/net/%s/speed", ifname);
    FILE *f = fopen(path, "r");
    int speed = 0;
    if (f) {
        if (fscanf(f, "%d", &speed) != 1 || speed < 0)
            speed = 0;
        fclose(f);
    }
    return speed;
}

/* Higher is better: a known wired speed beats Wi-Fi, which beats unknown. */
static int addr_score(const disc_addr_t *a)
{
    if (a->speed > 0)
        return a->speed;
    return a->wireless ? 300 : 100;
}

cJSON *netif_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    struct ifaddrs *ifs;
    if (getifaddrs(&ifs) < 0)
        return arr;
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) ||
            !(i->ifa_flags & IFF_UP))
            continue;
        char ip[64];
        inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof ip);
        bool wl;
        int sp = iface_speed(i->ifa_name, &wl);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "iface", i->ifa_name);
        cJSON_AddStringToObject(o, "addr", ip);
        cJSON_AddNumberToObject(o, "speed", sp);
        cJSON_AddBoolToObject(o, "wireless", wl);
        cJSON_AddItemToArray(arr, o);
    }
    freeifaddrs(ifs);
    return arr;
}

/* ---- socket ---- */

int discovery_open(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof one);
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &one, sizeof one);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(DISCOVERY_PORT),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        close(fd);
        return -1;
    }
    /* Join the group on every multicast-capable interface. */
    struct ifaddrs *ifs;
    if (getifaddrs(&ifs) == 0) {
        for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_MULTICAST))
                continue;
            struct ip_mreqn mr = { .imr_ifindex = (int)if_nametoindex(i->ifa_name) };
            inet_pton(AF_INET, MCAST_GROUP, &mr.imr_multiaddr);
            setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr);
        }
        freeifaddrs(ifs);
    }
    return fd;
}

void discovery_beacon(int fd)
{
    char msg[512];
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "n", g_cfg.name);
    cJSON_AddStringToObject(j, "fp", g_cfg.fp);
    cJSON_AddNumberToObject(j, "p", g_cfg.peer_port);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    int len = snprintf(msg, sizeof msg, BEACON_MAGIC "%s", s);
    free(s);
    if (len >= (int)sizeof msg)
        return;

    struct ifaddrs *ifs;
    if (getifaddrs(&ifs) < 0)
        return;
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP))
            continue;
        if ((i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr) {
            struct sockaddr_in b = *(struct sockaddr_in *)i->ifa_broadaddr;
            b.sin_port = htons(DISCOVERY_PORT);
            sendto(fd, msg, (size_t)len, 0, (struct sockaddr *)&b, sizeof b);
        }
        if (i->ifa_flags & IFF_MULTICAST) {
            struct ip_mreqn mr = { .imr_ifindex = (int)if_nametoindex(i->ifa_name) };
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &mr, sizeof mr);
            struct sockaddr_in m = { .sin_family = AF_INET, .sin_port = htons(DISCOVERY_PORT) };
            inet_pton(AF_INET, MCAST_GROUP, &m.sin_addr);
            sendto(fd, msg, (size_t)len, 0, (struct sockaddr *)&m, sizeof m);
        }
    }
    freeifaddrs(ifs);
}

static void record(const char *name, const char *fp, int port, const char *addr, const char *iface)
{
    double now = now_sec();
    pthread_mutex_lock(&disc_lock);
    dpeer_t *p = NULL;
    for (int i = 0; i < g_n_dpeers; i++)
        if (!strcmp(g_dpeers[i].fp, fp))
            p = &g_dpeers[i];
    if (!p) {
        if (g_n_dpeers == MAX_DPEERS) {
            pthread_mutex_unlock(&disc_lock);
            return;
        }
        p = &g_dpeers[g_n_dpeers++];
        memset(p, 0, sizeof *p);
        snprintf(p->fp, sizeof p->fp, "%s", fp);
        LOGI("discovered %s at %s (%s)", name, addr, iface);
    }
    snprintf(p->name, sizeof p->name, "%s", name);
    p->port = port;
    p->last_seen = now;

    disc_addr_t *a = NULL;
    for (int i = 0; i < p->n_addrs; i++)
        if (!strcmp(p->addrs[i].addr, addr))
            a = &p->addrs[i];
    if (!a) {
        if (p->n_addrs < MAX_ADDRS) {
            a = &p->addrs[p->n_addrs++];
        } else {   /* replace the stalest */
            a = &p->addrs[0];
            for (int i = 1; i < p->n_addrs; i++)
                if (p->addrs[i].last_seen < a->last_seen)
                    a = &p->addrs[i];
        }
        memset(a, 0, sizeof *a);
        snprintf(a->addr, sizeof a->addr, "%s", addr);
    }
    snprintf(a->iface, sizeof a->iface, "%s", iface);
    a->speed = iface_speed(iface, &a->wireless);
    a->last_seen = now;
    pthread_mutex_unlock(&disc_lock);
}

void discovery_handle(int fd)
{
    for (;;) {
        char buf[600], cbuf[256];
        struct sockaddr_in from;
        struct iovec iov = { buf, sizeof buf - 1 };
        struct msghdr mh = { .msg_name = &from, .msg_namelen = sizeof from, .msg_iov = &iov, .msg_iovlen = 1,
                             .msg_control = cbuf, .msg_controllen = sizeof cbuf };
        ssize_t n = recvmsg(fd, &mh, 0);
        if (n <= 0)
            return;
        buf[n] = 0;
        if (strncmp(buf, BEACON_MAGIC, strlen(BEACON_MAGIC)))
            continue;
        char iface[IF_NAMESIZE] = "?";
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
                struct in_pktinfo *pi = (struct in_pktinfo *)CMSG_DATA(c);
                if_indextoname((unsigned)pi->ipi_ifindex, iface);
            }
        }
        cJSON *j = cJSON_Parse(buf + strlen(BEACON_MAGIC));
        const cJSON *jn = cJSON_GetObjectItemCaseSensitive(j, "n");
        const cJSON *jf = cJSON_GetObjectItemCaseSensitive(j, "fp");
        const cJSON *jp = cJSON_GetObjectItemCaseSensitive(j, "p");
        if (cJSON_IsString(jn) && cJSON_IsString(jf) && cJSON_IsNumber(jp) && strlen(jf->valuestring) == 16 &&
            strcmp(jf->valuestring, g_cfg.fp) && jp->valueint > 0 && jp->valueint < 65536) {
            char addr[64];
            inet_ntop(AF_INET, &from.sin_addr, addr, sizeof addr);
            record(jn->valuestring, jf->valuestring, jp->valueint, addr, iface);
        }
        cJSON_Delete(j);
    }
}

void discovery_expire(void)
{
    double now = now_sec();
    pthread_mutex_lock(&disc_lock);
    for (int i = 0; i < g_n_dpeers;) {
        dpeer_t *p = &g_dpeers[i];
        for (int k = 0; k < p->n_addrs;) {
            if (now - p->addrs[k].last_seen > EXPIRE_SECS)
                p->addrs[k] = p->addrs[--p->n_addrs];
            else
                k++;
        }
        if (now - p->last_seen > EXPIRE_SECS || p->n_addrs == 0)
            g_dpeers[i] = g_dpeers[--g_n_dpeers];
        else
            i++;
    }
    pthread_mutex_unlock(&disc_lock);
}

static const disc_addr_t *best_addr(const dpeer_t *p)
{
    const disc_addr_t *b = NULL;
    for (int i = 0; i < p->n_addrs; i++)
        if (!b || addr_score(&p->addrs[i]) > addr_score(b))
            b = &p->addrs[i];
    return b;
}

cJSON *discovery_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    pthread_mutex_lock(&disc_lock);
    for (int i = 0; i < g_n_dpeers; i++) {
        dpeer_t *p = &g_dpeers[i];
        const disc_addr_t *a = best_addr(p);
        if (!a)
            continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", p->name);
        cJSON_AddStringToObject(o, "fp", p->fp);
        cJSON_AddNumberToObject(o, "port", p->port);
        cJSON_AddStringToObject(o, "addr", a->addr);
        cJSON_AddStringToObject(o, "iface", a->iface);
        cJSON_AddNumberToObject(o, "speed", a->speed);
        cJSON_AddBoolToObject(o, "wireless", a->wireless);
        cJSON_AddBoolToObject(o, "paired", peers_find(p->fp, NULL));
        cJSON_AddItemToArray(arr, o);
    }
    pthread_mutex_unlock(&disc_lock);
    return arr;
}

bool discovery_lookup(const char *key, char *host, size_t hn, int *port, char *fp, char *name, size_t nn)
{
    bool found = false;
    size_t kl = strlen(key);
    pthread_mutex_lock(&disc_lock);
    for (int i = 0; i < g_n_dpeers && !found; i++) {
        dpeer_t *p = &g_dpeers[i];
        if (strcasecmp(p->name, key) && !(kl >= 4 && !strncmp(p->fp, key, kl)))
            continue;
        const disc_addr_t *a = best_addr(p);
        if (!a)
            continue;
        snprintf(host, hn, "%s", a->addr);
        *port = p->port;
        if (fp)
            snprintf(fp, 17, "%s", p->fp);
        if (name)
            snprintf(name, nn, "%s", p->name);
        found = true;
    }
    pthread_mutex_unlock(&disc_lock);
    return found;
}
