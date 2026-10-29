#include "api.h"
#include "clap.h"
#include "config.h"
#include "compress.h"
#include "discovery.h"
#include "peer.h"
#include "sender.h"
#include "transfer.h"

#include <dirent.h>
#include <netdb.h>
#include <stdarg.h>
#include <sys/stat.h>

#define PAIR_TTL 300
#define CODE_TTL 600

static cJSON *error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static cJSON *error(const char *fmt, ...)
{
    char msg[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", msg);
    return j;
}

static const char *arg_str(const cJSON *a, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(a, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int arg_int(const cJSON *a, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(a, k);
    if (cJSON_IsNumber(v))
        return v->valueint;
    if (cJSON_IsString(v) && *v->valuestring)
        return atoi(v->valuestring);
    return def;
}

static cJSON *ok(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    return j;
}

static cJSON *info_json(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "name", g_cfg.name);
    cJSON_AddStringToObject(j, "fp", g_cfg.fp);
    cJSON_AddStringToObject(j, "version", CLAP_VERSION);
    cJSON_AddNumberToObject(j, "peer_port", g_cfg.peer_port);
    cJSON_AddNumberToObject(j, "web_port", g_cfg.web_port);
    cJSON_AddStringToObject(j, "download_dir", g_cfg.download_dir);
    cJSON_AddBoolToObject(j, "auto_accept", g_cfg.auto_accept);
    cJSON_AddNumberToObject(j, "streams", g_cfg.streams);
    cJSON_AddNumberToObject(j, "compress", g_cfg.compress);
    cJSON_AddStringToObject(j, "cipher", crypt_aes_available() ? "AES-256-GCM" : "ChaCha20-Poly1305");
    cJSON_AddStringToObject(j, "home", getenv("HOME") ? getenv("HOME") : "/");
    cJSON_AddItemToObject(j, "interfaces", netif_json());
    return j;
}

static cJSON *peers_json(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON *disc = discovery_json();
    cJSON_AddItemToObject(j, "discovered", disc);
    cJSON *paired = cJSON_AddArrayToObject(j, "paired");
    size_t n;
    peer_t *ps = peers_list(&n);
    for (size_t i = 0; i < n; i++) {
        char host[256];
        int port;
        bool online = discovery_lookup(ps[i].fp, host, sizeof host, &port, NULL, NULL, 0);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", ps[i].name);
        cJSON_AddStringToObject(o, "fp", ps[i].fp);
        cJSON_AddStringToObject(o, "host", online ? host : ps[i].host);
        cJSON_AddNumberToObject(o, "port", online ? port : ps[i].port);
        cJSON_AddBoolToObject(o, "online", online);
        cJSON_AddNumberToObject(o, "paired_at", (double)ps[i].paired_at);
        cJSON_AddItemToArray(paired, o);
    }
    free(ps);
    return j;
}

static bool host_resolves(const char *host)
{
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *res;
    if (getaddrinfo(host, NULL, &hints, &res) != 0)
        return false;
    freeaddrinfo(res);
    return true;
}

/* A name that is neither a discovered device nor a host name: say so, and
 * list what is on the network (long model-number names are easy to mistype). */
static void unknown_peer(const char *to, char *err, size_t errn)
{
    cJSON *list = discovery_json();
    char names[400] = "";
    size_t used = 0;
    const cJSON *d;
    cJSON_ArrayForEach(d, list) {
        const cJSON *nm = cJSON_GetObjectItem(d, "name");
        if (cJSON_IsString(nm) && used < sizeof names - 1)
            used += (size_t)snprintf(names + used, sizeof names - used, "%s%s", used ? ", " : "", nm->valuestring);
    }
    cJSON_Delete(list);
    if (names[0])
        snprintf(err, errn, "no device called '%s' on this network. Found: %s "
                 "(a fingerprint prefix or IP address works too)", to, names);
    else
        snprintf(err, errn, "no device called '%s' found — is thunderclap running there? "
                 "Check `clap peers`, or use its IP address", to);
}

/* Works out where to connect and how to authenticate for "--to X". */
static int resolve_target(const char *to, bool with_code, send_req_t *rq, char *err, size_t errn)
{
    peer_t p;
    char fp[17] = "";
    if (!with_code && peers_find(to, &p)) {
        rq->have_pk = true;
        memcpy(rq->peer_pk, p.pk, KEY_LEN);
        snprintf(rq->peer_name, sizeof rq->peer_name, "%s", p.name);
        if (!discovery_lookup(p.fp, rq->host, sizeof rq->host, &rq->port, NULL, NULL, 0)) {
            if (!p.host[0]) {
                snprintf(err, errn, "%s is paired but its address is unknown — use host:port", p.name);
                return -1;
            }
            snprintf(rq->host, sizeof rq->host, "%s", p.host);
            rq->port = p.port ? p.port : DEFAULT_PEER_PORT;
        }
        return 0;
    }
    if (discovery_lookup(to, rq->host, sizeof rq->host, &rq->port, fp, rq->peer_name, sizeof rq->peer_name)) {
        if (with_code)
            return 0;
        if (peers_find(fp, &p)) {
            rq->have_pk = true;
            memcpy(rq->peer_pk, p.pk, KEY_LEN);
            return 0;
        }
        snprintf(err, errn, "%s is not paired — run `clap pair` on it, or use a one-shot code", rq->peer_name);
        return -1;
    }
    if (parse_host_port(to, rq->host, sizeof rq->host, &rq->port, DEFAULT_PEER_PORT) < 0 ||
        !host_resolves(rq->host)) {
        unknown_peer(to, err, errn);
        return -1;
    }
    if (with_code)
        return 0;
    /* A raw address: is it a paired peer we know by that address? */
    size_t n;
    peer_t *ps = peers_list(&n);
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(ps[i].host, rq->host) && (ps[i].port == rq->port || !ps[i].port)) {
            rq->have_pk = true;
            memcpy(rq->peer_pk, ps[i].pk, KEY_LEN);
            snprintf(rq->peer_name, sizeof rq->peer_name, "%s", ps[i].name);
            break;
        }
    }
    free(ps);
    if (!rq->have_pk) {
        snprintf(err, errn, "no paired peer at %s — pair first with `clap pair`, or use a one-shot code", to);
        return -1;
    }
    return 0;
}

static int parse_compress(const cJSON *args)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, "compress");
    if (cJSON_IsNumber(v))
        return MAX(-1, MIN(v->valueint, 19));
    if (cJSON_IsString(v)) {
        if (!strcmp(v->valuestring, "auto"))
            return COMPRESS_AUTO;
        if (!strcmp(v->valuestring, "off") || !strcmp(v->valuestring, "none"))
            return COMPRESS_OFF;
        int l = atoi(v->valuestring);
        if (l >= 1 && l <= 19)
            return l;
    }
    return g_cfg.compress;
}

static cJSON *cmd_send(const cJSON *args, bool bench)
{
    const char *to = arg_str(args, "to");
    const char *code = arg_str(args, "code");
    if (!to || !*to)
        return error("missing 'to'");
    send_req_t rq = { 0 };
    char err[600];
    if (resolve_target(to, code && *code, &rq, err, sizeof err) < 0)
        return error("%s", err);
    if (code)
        snprintf(rq.code, sizeof rq.code, "%s", code);
    rq.streams = arg_int(args, "streams", g_cfg.streams);
    rq.compress = parse_compress(args);
    if (bench) {
        rq.bench = true;
        rq.bench_secs = MAX(1, MIN(arg_int(args, "secs", 10), 120));
        rq.compress = COMPRESS_OFF;
    } else {
        const cJSON *paths = cJSON_GetObjectItemCaseSensitive(args, "paths");
        if (!cJSON_IsArray(paths) || cJSON_GetArraySize(paths) == 0)
            return error("nothing to send");
        int n = cJSON_GetArraySize(paths);
        rq.paths = xcalloc((size_t)n, sizeof *rq.paths);
        const cJSON *p;
        cJSON_ArrayForEach(p, paths) {
            struct stat st;
            if (!cJSON_IsString(p) || lstat(p->valuestring, &st) < 0) {
                for (int i = 0; i < rq.n_paths; i++)
                    free(rq.paths[i]);
                free(rq.paths);
                return error("cannot access %s", cJSON_IsString(p) ? p->valuestring : "?");
            }
            rq.paths[rq.n_paths++] = xstrdup(p->valuestring);
        }
    }
    transfer_t *t = sender_start(&rq);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "id", t->id);
    transfer_put(t);
    return j;
}

static cJSON *cmd_transfer(const cJSON *args)
{
    transfer_t *t = transfer_get(arg_int(args, "id", -1));
    if (!t)
        return error("no such transfer");
    cJSON *j = transfer_json(t, true);
    transfer_put(t);
    return j;
}

static cJSON *cmd_decide(const cJSON *args, int what)
{
    transfer_t *t = transfer_get(arg_int(args, "id", -1));
    if (!t)
        return error("no such transfer");
    pthread_mutex_lock(&g_xfer_lock);
    bool finished = transfer_finished(t);
    if (what == 0) {
        atomic_store(&t->cancel, true);
    } else if (!t->outgoing && t->state == TS_WAITING) {
        t->decision = what;
    } else {
        finished = true;
    }
    pthread_cond_broadcast(&t->cond);
    pthread_mutex_unlock(&g_xfer_lock);
    transfer_put(t);
    if (finished)
        return error(what == 0 ? "transfer already finished" : "transfer is not waiting for a decision");
    return ok();
}

static cJSON *cmd_pair_join(const cJSON *args)
{
    const char *to = arg_str(args, "to"), *code = arg_str(args, "code");
    if (!to || !code)
        return error("usage: pair_join {to, code}");
    char host[256];
    int port;
    peer_t p;
    char err[600];
    if (!discovery_lookup(to, host, sizeof host, &port, NULL, NULL, 0) &&
        (parse_host_port(to, host, sizeof host, &port, DEFAULT_PEER_PORT) < 0 || !host_resolves(host))) {
        unknown_peer(to, err, sizeof err);
        return error("%s", err);
    }
    if (pair_join(host, port, code, &p, err, sizeof err) < 0)
        return error("%s", err);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "name", p.name);
    cJSON_AddStringToObject(j, "fp", p.fp);
    return j;
}

static cJSON *code_json(const char *code, int ttl)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "code", code);
    cJSON_AddNumberToObject(j, "ttl", ttl);
    cJSON_AddStringToObject(j, "name", g_cfg.name);
    cJSON_AddStringToObject(j, "fp", g_cfg.fp);
    return j;
}

static int cmp_entry(const void *a, const void *b)
{
    const cJSON *x = *(const cJSON *const *)a, *y = *(const cJSON *const *)b;
    bool dx = cJSON_IsTrue(cJSON_GetObjectItem(x, "dir")), dy = cJSON_IsTrue(cJSON_GetObjectItem(y, "dir"));
    if (dx != dy)
        return dx ? -1 : 1;
    return strcasecmp(cJSON_GetObjectItem(x, "name")->valuestring, cJSON_GetObjectItem(y, "name")->valuestring);
}

static cJSON *cmd_ls(const cJSON *args)
{
    const char *path = arg_str(args, "path");
    if (!path || !*path)
        path = getenv("HOME") ? getenv("HOME") : "/";
    bool hidden = cJSON_IsTrue(cJSON_GetObjectItem(args, "hidden"));
    char *real = realpath(path, NULL);
    if (!real)
        return error("cannot open %s: %s", path, strerror(errno));
    DIR *d = opendir(real);
    if (!d) {
        cJSON *e = error("cannot open %s: %s", real, strerror(errno));
        free(real);
        return e;
    }
    size_t n = 0, cap = 64;
    cJSON **items = xmalloc(cap * sizeof *items);
    struct dirent *de;
    while ((de = readdir(d)) && n < 20000) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (!hidden && de->d_name[0] == '.')
            continue;
        struct stat st;
        if (fstatat(dirfd(d), de->d_name, &st, 0) < 0)
            continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", de->d_name);
        cJSON_AddBoolToObject(o, "dir", S_ISDIR(st.st_mode));
        cJSON_AddNumberToObject(o, "size", (double)st.st_size);
        cJSON_AddNumberToObject(o, "mtime", (double)st.st_mtime);
        if (n == cap)
            items = xrealloc(items, (cap *= 2) * sizeof *items);
        items[n++] = o;
    }
    closedir(d);
    qsort(items, n, sizeof *items, cmp_entry);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "path", real);
    char *parent = xstrdup(real);
    char *slash = strrchr(parent, '/');
    if (slash)
        slash == parent ? (void)(slash[1] = 0) : (void)(*slash = 0);
    cJSON_AddStringToObject(j, "parent", parent);
    free(parent);
    cJSON *arr = cJSON_AddArrayToObject(j, "entries");
    for (size_t i = 0; i < n; i++)
        cJSON_AddItemToArray(arr, items[i]);
    free(items);
    free(real);
    return j;
}

static cJSON *cmd_config_set(const cJSON *args)
{
    const char *name = arg_str(args, "name");
    const char *dir = arg_str(args, "download_dir");
    const cJSON *aa = cJSON_GetObjectItemCaseSensitive(args, "auto_accept");
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(args, "streams");
    if (name && *name)
        snprintf(g_cfg.name, sizeof g_cfg.name, "%s", name);
    if (dir && *dir) {
        if (dir[0] != '/')
            return error("download_dir must be an absolute path");
        if (mkdir_p(dir, 0755) < 0)
            return error("cannot create %s: %s", dir, strerror(errno));
        snprintf(g_cfg.download_dir, sizeof g_cfg.download_dir, "%s", dir);
    }
    if (cJSON_IsBool(aa))
        g_cfg.auto_accept = cJSON_IsTrue(aa);
    if (cJSON_IsNumber(st))
        g_cfg.streams = MAX(1, MIN(st->valueint, MAX_STREAMS));
    if (cJSON_GetObjectItemCaseSensitive(args, "compress"))
        g_cfg.compress = parse_compress(args);
    if (config_save() < 0)
        return error("cannot save config: %s", strerror(errno));
    return info_json();
}

cJSON *api_state(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddItemToObject(j, "info", info_json());
    cJSON_AddItemToObject(j, "peers", peers_json());
    cJSON_AddItemToObject(j, "transfers", transfers_json(true));
    cJSON_AddNumberToObject(j, "pair_ttl", pair_code_ttl());
    cJSON_AddNumberToObject(j, "recv_ttl", recv_code_ttl());
    return j;
}

cJSON *api_call(const char *cmd, const cJSON *args)
{
    if (!cmd)
        return error("missing command");
    if (!strcmp(cmd, "info"))
        return info_json();
    if (!strcmp(cmd, "state"))
        return api_state();
    if (!strcmp(cmd, "peers"))
        return peers_json();
    if (!strcmp(cmd, "send"))
        return cmd_send(args, false);
    if (!strcmp(cmd, "bench"))
        return cmd_send(args, true);
    if (!strcmp(cmd, "transfers")) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddItemToObject(j, "transfers", transfers_json(false));
        return j;
    }
    if (!strcmp(cmd, "transfer"))
        return cmd_transfer(args);
    if (!strcmp(cmd, "history")) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddItemToObject(j, "history", history_json());
        return j;
    }
    if (!strcmp(cmd, "cancel"))
        return cmd_decide(args, 0);
    if (!strcmp(cmd, "accept"))
        return cmd_decide(args, 1);
    if (!strcmp(cmd, "reject"))
        return cmd_decide(args, -1);
    if (!strcmp(cmd, "pair_start")) {
        char code[15];
        pair_code_arm(code, PAIR_TTL);
        return code_json(code, PAIR_TTL);
    }
    if (!strcmp(cmd, "pair_join"))
        return cmd_pair_join(args);
    if (!strcmp(cmd, "unpair")) {
        const char *who = arg_str(args, "peer");
        if (!who || !peers_remove(who))
            return error("no such paired peer");
        return ok();
    }
    if (!strcmp(cmd, "recv_code")) {
        char code[15];
        recv_code_arm(code, CODE_TTL);
        return code_json(code, CODE_TTL);
    }
    if (!strcmp(cmd, "ls"))
        return cmd_ls(args);
    if (!strcmp(cmd, "config_set"))
        return cmd_config_set(args);
    return error("unknown command '%s'", cmd);
}
