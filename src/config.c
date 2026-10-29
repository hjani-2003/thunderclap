#include "config.h"
#include "compress.h"

#include <cjson/cJSON.h>
#include <fcntl.h>
#include <sys/stat.h>

config_t g_cfg;

static pthread_mutex_t peers_lock = PTHREAD_MUTEX_INITIALIZER;
static peer_t *g_peers;
static size_t g_n_peers;

void config_path(const char *file, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", g_cfg.home, file);
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = xmalloc((size_t)n + 1);
    size_t r = fread(s, 1, (size_t)n, f);
    s[r] = 0;
    fclose(f);
    return s;
}

/* Writes atomically via a temp file + rename. */
static int write_file(const char *path, const char *data, mode_t mode)
{
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return -1;
    int rc = write_full(fd, data, strlen(data));
    close(fd);
    if (rc == 0)
        rc = rename(tmp, path);
    return rc;
}

static void copy_str(char *dst, size_t n, const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring[0])
        snprintf(dst, n, "%s", v->valuestring);
}

static void copy_int(int *dst, const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v))
        *dst = v->valueint;
}

int config_load(const char *home)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    const char *env = getenv("CLAP_HOME");
    const char *h = getenv("HOME");
    if (!h)
        h = "/tmp";
    if (home)
        snprintf(g_cfg.home, sizeof g_cfg.home, "%s", home);
    else if (env && *env)
        snprintf(g_cfg.home, sizeof g_cfg.home, "%s", env);
    else if (getenv("XDG_CONFIG_HOME"))
        snprintf(g_cfg.home, sizeof g_cfg.home, "%s/thunderclap", getenv("XDG_CONFIG_HOME"));
    else
        snprintf(g_cfg.home, sizeof g_cfg.home, "%s/.config/thunderclap", h);
    if (mkdir_p(g_cfg.home, 0700) < 0)
        return -1;

    /* A custom home gets its own socket so several daemons can coexist. */
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!home && !(env && *env) && rt && *rt)
        snprintf(g_cfg.sock_path, sizeof g_cfg.sock_path, "%s/clap.sock", rt);
    else
        snprintf(g_cfg.sock_path, sizeof g_cfg.sock_path, "%s/clap.sock", g_cfg.home);
    if (strlen(g_cfg.sock_path) >= 100) {
        /* sun_path holds ~108 bytes; use a short per-home name instead. */
        uint8_t h[8];
        char hex[17];
        crypto_generichash(h, sizeof h, (const uint8_t *)g_cfg.home, strlen(g_cfg.home), NULL, 0);
        hex_encode(h, sizeof h, hex);
        snprintf(g_cfg.sock_path, sizeof g_cfg.sock_path, "%s/clap-%s.sock", rt && *rt ? rt : "/tmp", hex);
    }

    /* defaults */
    char host[64] = "linux";
    gethostname(host, sizeof host);
    snprintf(g_cfg.name, sizeof g_cfg.name, "%s", host);
    snprintf(g_cfg.download_dir, sizeof g_cfg.download_dir, "%s/Downloads/thunderclap", h);
    g_cfg.peer_port = DEFAULT_PEER_PORT;
    g_cfg.web_port = DEFAULT_WEB_PORT;
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    g_cfg.streams = (int)MAX(2, MIN(cores, 8));
    g_cfg.compress = COMPRESS_AUTO;
    g_cfg.auto_accept = true;

    char path[1200];
    config_path("config.json", path, sizeof path);
    char *txt = slurp(path);
    if (txt) {
        cJSON *j = cJSON_Parse(txt);
        if (j) {
            copy_str(g_cfg.name, sizeof g_cfg.name, j, "name");
            copy_str(g_cfg.download_dir, sizeof g_cfg.download_dir, j, "download_dir");
            copy_int(&g_cfg.peer_port, j, "peer_port");
            copy_int(&g_cfg.web_port, j, "web_port");
            copy_int(&g_cfg.streams, j, "streams");
            copy_int(&g_cfg.compress, j, "compress");
            const cJSON *aa = cJSON_GetObjectItemCaseSensitive(j, "auto_accept");
            if (cJSON_IsBool(aa))
                g_cfg.auto_accept = cJSON_IsTrue(aa);
            cJSON_Delete(j);
        } else {
            LOGW("could not parse %s, using defaults", path);
        }
        free(txt);
    } else {
        config_save();
    }

    config_path("identity.key", path, sizeof path);
    if (identity_load(path, &g_cfg.id) < 0) {
        LOGE("cannot load or create identity at %s: %s", path, strerror(errno));
        return -1;
    }
    key_fingerprint(g_cfg.id.pk, g_cfg.fp);

    config_path("web_token", path, sizeof path);
    txt = slurp(path);
    if (txt && strlen(txt) >= 32) {
        snprintf(g_cfg.web_token, sizeof g_cfg.web_token, "%.64s", txt);
        g_cfg.web_token[strcspn(g_cfg.web_token, "\r\n")] = 0;
    } else {
        uint8_t raw[24];
        randombytes_buf(raw, sizeof raw);
        hex_encode(raw, sizeof raw, g_cfg.web_token);
        write_file(path, g_cfg.web_token, 0600);
    }
    free(txt);
    return peers_load();
}

int config_save(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "name", g_cfg.name);
    cJSON_AddStringToObject(j, "download_dir", g_cfg.download_dir);
    cJSON_AddNumberToObject(j, "peer_port", g_cfg.peer_port);
    cJSON_AddNumberToObject(j, "web_port", g_cfg.web_port);
    cJSON_AddNumberToObject(j, "streams", g_cfg.streams);
    cJSON_AddNumberToObject(j, "compress", g_cfg.compress);
    cJSON_AddBoolToObject(j, "auto_accept", g_cfg.auto_accept);
    char *s = cJSON_Print(j);
    cJSON_Delete(j);
    char path[1200];
    config_path("config.json", path, sizeof path);
    int rc = write_file(path, s, 0600);
    free(s);
    return rc;
}

/* ---- peers ---- */

static int peers_save_locked(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < g_n_peers; i++) {
        peer_t *p = &g_peers[i];
        char pkhex[2 * KEY_LEN + 1];
        hex_encode(p->pk, KEY_LEN, pkhex);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", p->name);
        cJSON_AddStringToObject(o, "pk", pkhex);
        cJSON_AddStringToObject(o, "host", p->host);
        cJSON_AddNumberToObject(o, "port", p->port);
        cJSON_AddNumberToObject(o, "paired_at", (double)p->paired_at);
        cJSON_AddItemToArray(arr, o);
    }
    char *s = cJSON_Print(arr);
    cJSON_Delete(arr);
    char path[1200];
    config_path("peers.json", path, sizeof path);
    int rc = write_file(path, s, 0600);
    free(s);
    return rc;
}

int peers_load(void)
{
    char path[1200];
    config_path("peers.json", path, sizeof path);
    char *txt = slurp(path);
    if (!txt)
        return 0;
    cJSON *arr = cJSON_Parse(txt);
    free(txt);
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        LOGW("could not parse %s", path);
        return 0;
    }
    pthread_mutex_lock(&peers_lock);
    const cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        peer_t p = { 0 };
        const cJSON *pk = cJSON_GetObjectItemCaseSensitive(o, "pk");
        if (!cJSON_IsString(pk) || hex_decode(pk->valuestring, p.pk, KEY_LEN) < 0)
            continue;
        copy_str(p.name, sizeof p.name, o, "name");
        copy_str(p.host, sizeof p.host, o, "host");
        copy_int(&p.port, o, "port");
        const cJSON *pa = cJSON_GetObjectItemCaseSensitive(o, "paired_at");
        p.paired_at = cJSON_IsNumber(pa) ? (int64_t)pa->valuedouble : 0;
        key_fingerprint(p.pk, p.fp);
        g_peers = xrealloc(g_peers, (g_n_peers + 1) * sizeof *g_peers);
        g_peers[g_n_peers++] = p;
    }
    pthread_mutex_unlock(&peers_lock);
    cJSON_Delete(arr);
    return 0;
}

bool peers_find_pk(const uint8_t pk[KEY_LEN], peer_t *out)
{
    bool found = false;
    pthread_mutex_lock(&peers_lock);
    for (size_t i = 0; i < g_n_peers && !found; i++) {
        if (sodium_memcmp(g_peers[i].pk, pk, KEY_LEN) == 0) {
            if (out)
                *out = g_peers[i];
            found = true;
        }
    }
    pthread_mutex_unlock(&peers_lock);
    return found;
}

bool peers_find(const char *s, peer_t *out)
{
    bool found = false;
    size_t sl = strlen(s);
    pthread_mutex_lock(&peers_lock);
    for (size_t i = 0; i < g_n_peers && !found; i++) {
        if (!strcasecmp(g_peers[i].name, s) || (sl >= 4 && !strncmp(g_peers[i].fp, s, sl))) {
            if (out)
                *out = g_peers[i];
            found = true;
        }
    }
    pthread_mutex_unlock(&peers_lock);
    return found;
}

void peers_upsert(const peer_t *p)
{
    pthread_mutex_lock(&peers_lock);
    size_t i;
    for (i = 0; i < g_n_peers; i++)
        if (!memcmp(g_peers[i].pk, p->pk, KEY_LEN))
            break;
    if (i == g_n_peers)
        g_peers = xrealloc(g_peers, (++g_n_peers) * sizeof *g_peers);
    g_peers[i] = *p;
    key_fingerprint(g_peers[i].pk, g_peers[i].fp);
    peers_save_locked();
    pthread_mutex_unlock(&peers_lock);
}

void peers_set_host(const uint8_t pk[KEY_LEN], const char *host, int port)
{
    pthread_mutex_lock(&peers_lock);
    for (size_t i = 0; i < g_n_peers; i++) {
        if (!memcmp(g_peers[i].pk, pk, KEY_LEN)) {
            if (strcmp(g_peers[i].host, host) || g_peers[i].port != port) {
                snprintf(g_peers[i].host, sizeof g_peers[i].host, "%s", host);
                g_peers[i].port = port;
                peers_save_locked();
            }
            break;
        }
    }
    pthread_mutex_unlock(&peers_lock);
}

bool peers_remove(const char *s)
{
    bool removed = false;
    size_t sl = strlen(s);
    pthread_mutex_lock(&peers_lock);
    for (size_t i = 0; i < g_n_peers; i++) {
        if (!strcasecmp(g_peers[i].name, s) || (sl >= 4 && !strncmp(g_peers[i].fp, s, sl))) {
            memmove(&g_peers[i], &g_peers[i + 1], (g_n_peers - i - 1) * sizeof *g_peers);
            g_n_peers--;
            removed = true;
            break;
        }
    }
    if (removed)
        peers_save_locked();
    pthread_mutex_unlock(&peers_lock);
    return removed;
}

peer_t *peers_list(size_t *n)
{
    pthread_mutex_lock(&peers_lock);
    peer_t *copy = xmalloc((g_n_peers + 1) * sizeof *copy);
    memcpy(copy, g_peers, g_n_peers * sizeof *copy);
    *n = g_n_peers;
    pthread_mutex_unlock(&peers_lock);
    return copy;
}
