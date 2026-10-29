#include "daemon.h"
#include "api.h"
#include "config.h"
#include "discovery.h"
#include "peer.h"
#include "transfer.h"

#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#define IPC_MAX_REQ (1 << 20)

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ---- thread plumbing ---- */

typedef struct {
    int fd;
    int kind;
    struct sockaddr_storage addr;
} conn_job_t;

enum { JOB_PEER, JOB_HTTP, JOB_IPC };

static void ipc_serve(int fd);

static void *conn_thread(void *arg)
{
    conn_job_t *job = arg;
    switch (job->kind) {
    case JOB_PEER:
        serve_connection(job->fd, &job->addr);
        break;
    case JOB_HTTP:
        http_serve(job->fd);
        break;
    case JOB_IPC:
        ipc_serve(job->fd);
        break;
    }
    free(job);
    return NULL;
}

static void spawn(int kind, int listen_fd)
{
    conn_job_t *job = xcalloc(1, sizeof *job);
    socklen_t al = sizeof job->addr;
    job->fd = accept4(listen_fd, (struct sockaddr *)&job->addr, &al, SOCK_CLOEXEC);
    if (job->fd < 0) {
        free(job);
        return;
    }
    job->kind = kind;
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, conn_thread, job) != 0) {
        close(job->fd);
        free(job);
    }
    pthread_attr_destroy(&attr);
}

/* ---- unix socket API: one JSON request line, one JSON reply line ---- */

static void ipc_serve(int fd)
{
    char *buf = xmalloc(IPC_MAX_REQ + 1);
    size_t n = 0;
    while (n < IPC_MAX_REQ) {
        ssize_t r = read(fd, buf + n, IPC_MAX_REQ - n);
        if (r <= 0)
            break;
        n += (size_t)r;
        if (memchr(buf + n - r, '\n', (size_t)r))
            break;
    }
    buf[n] = 0;
    cJSON *req = cJSON_Parse(buf);
    free(buf);
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(req, "cmd");
    /* Only reachable over the user-private socket, never over HTTP. */
    if (cJSON_IsString(cmd) && !strcmp(cmd->valuestring, "shutdown")) {
        write_full(fd, "{\"ok\":true}\n", 12);
        g_stop = 1;
        cJSON_Delete(req);
        close(fd);
        return;
    }
    cJSON *resp = api_call(cJSON_IsString(cmd) ? cmd->valuestring : NULL,
                           cJSON_GetObjectItemCaseSensitive(req, "args"));
    char *out = cJSON_PrintUnformatted(resp);
    write_full(fd, out, strlen(out));
    write_full(fd, "\n", 1);
    free(out);
    cJSON_Delete(resp);
    cJSON_Delete(req);
    close(fd);
}

static int ipc_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", g_cfg.sock_path);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    return fd;
}

bool ipc_ping(void)
{
    int fd = ipc_connect();
    if (fd < 0)
        return false;
    close(fd);
    return true;
}

cJSON *ipc_call(const char *cmd, cJSON *args)
{
    int fd = ipc_connect();
    if (fd < 0)
        return NULL;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "cmd", cmd);
    if (args)
        cJSON_AddItemReferenceToObject(req, "args", args);
    char *s = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    int rc = write_full(fd, s, strlen(s)) | write_full(fd, "\n", 1);
    free(s);
    if (rc < 0) {
        close(fd);
        return NULL;
    }
    buf_t b = { 0 };
    char tmp[8192];
    ssize_t r;
    while ((r = read(fd, tmp, sizeof tmp)) > 0)
        buf_put(&b, tmp, (size_t)r);
    close(fd);
    buf_u8(&b, 0);
    cJSON *resp = cJSON_Parse((char *)b.data);
    buf_free(&b);
    return resp;
}

static int ipc_listen(void)
{
    if (ipc_ping()) {
        LOGE("a thunderclap daemon is already running (%s)", g_cfg.sock_path);
        return -1;
    }
    unlink(g_cfg.sock_path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", g_cfg.sock_path);
    mode_t old = umask(077);   /* only this user may talk to the daemon */
    int rc = bind(fd, (struct sockaddr *)&sa, sizeof sa);
    umask(old);
    if (rc < 0 || listen(fd, 32) < 0) {
        LOGE("cannot listen on %s: %s", g_cfg.sock_path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- main loop ---- */

int daemon_main(void)
{
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int ipc_fd = ipc_listen();
    if (ipc_fd < 0)
        return 1;
    int peer_fd = tcp_listen(NULL, g_cfg.peer_port);
    if (peer_fd < 0) {
        LOGE("cannot listen on port %d: %s", g_cfg.peer_port, strerror(errno));
        unlink(g_cfg.sock_path);
        return 1;
    }
    int web_fd = g_cfg.web_port > 0 ? tcp_listen("127.0.0.1", g_cfg.web_port) : -1;
    if (g_cfg.web_port > 0 && web_fd < 0)
        LOGW("web UI disabled: cannot listen on 127.0.0.1:%d: %s", g_cfg.web_port, strerror(errno));
    int disc_fd = discovery_open();
    if (disc_fd < 0)
        LOGW("LAN discovery disabled: %s", strerror(errno));

    char pidpath[1200];
    config_path("daemon.pid", pidpath, sizeof pidpath);
    FILE *pf = fopen(pidpath, "w");
    if (pf) {
        fprintf(pf, "%d\n", (int)getpid());
        fclose(pf);
    }

    transfers_init();
    mkdir_p(g_cfg.download_dir, 0755);
    LOGI("thunderclap %s — %s (%s)", CLAP_VERSION, g_cfg.name, g_cfg.fp);
    LOGI("peers: port %d   web UI: http://127.0.0.1:%d   downloads: %s", g_cfg.peer_port, g_cfg.web_port,
         g_cfg.download_dir);

    double next_beacon = 0;
    while (!g_stop) {
        struct pollfd pfd[4] = {
            { .fd = peer_fd, .events = POLLIN },
            { .fd = web_fd, .events = POLLIN },
            { .fd = ipc_fd, .events = POLLIN },
            { .fd = disc_fd, .events = POLLIN },
        };
        int pr = poll(pfd, 4, 500);
        if (pr > 0) {
            if (pfd[0].revents & POLLIN)
                spawn(JOB_PEER, peer_fd);
            if (pfd[1].revents & POLLIN)
                spawn(JOB_HTTP, web_fd);
            if (pfd[2].revents & POLLIN)
                spawn(JOB_IPC, ipc_fd);
            if (pfd[3].revents & POLLIN)
                discovery_handle(disc_fd);
        }
        transfers_tick();
        if (disc_fd >= 0 && now_sec() >= next_beacon) {
            discovery_beacon(disc_fd);
            discovery_expire();
            next_beacon = now_sec() + 2.0;
        }
    }
    LOGI("shutting down");
    unlink(g_cfg.sock_path);
    unlink(pidpath);
    return 0;
}
