/* beam — command-line front-end. Everything except `beam daemon` talks to
 * the background daemon over a unix socket, starting it when needed. */
#include "beam.h"
#include "config.h"
#include "compress.h"
#include "daemon.h"

#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>

static const char *g_home_override;
static volatile sig_atomic_t g_interrupted;

static void on_sigint(int sig)
{
    (void)sig;
    g_interrupted = 1;
}

static void usage(void)
{
    puts("beam " BEAM_VERSION " — fast, encrypted file transfer between Linux machines\n"
         "\n"
         "usage: beam [--home DIR] <command> [options]\n"
         "\n"
         "  send <path>... --to <peer>   send files/folders to a paired peer\n"
         "       [--streams N] [--compress auto|off|1-19] [--code CODE] [--detach]\n"
         "  recv --code                  show a one-shot code so an unpaired machine can send to you\n"
         "  pair                         show a pairing code (run on one machine)\n"
         "  pair <peer> <code>           pair with that machine (run on the other)\n"
         "  unpair <peer>                forget a paired peer\n"
         "  peers                        discovered and paired peers\n"
         "  ls                           list transfers (alias: transfers)\n"
         "  watch <id>                   follow a transfer's progress\n"
         "  accept|reject|cancel <id>    act on a transfer\n"
         "  history                      past transfers\n"
         "  bench <peer> [--secs N]      measure raw link throughput (no disk I/O)\n"
         "  ui                           open the web UI in your browser\n"
         "  status                       daemon and device info\n"
         "  config [key value]           show or change settings\n"
         "                               keys: name, download_dir, auto_accept, streams, compress\n"
         "  daemon [--port N] [--web N] [--dir DIR] [--name NAME] [-v]\n"
         "                               run the daemon in the foreground\n"
         "  stop                         stop the daemon\n"
         "\n"
         "<peer> is a paired or discovered device name, a fingerprint prefix, or host[:port].");
}

/* ---- daemon management ---- */

static bool start_daemon(void)
{
    if (ipc_ping())
        return true;
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n < 0)
        return false;
    exe[n] = 0;
    char logpath[1200];
    config_path("daemon.log", logpath, sizeof logpath);

    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        setsid();
        int fd = open("/dev/null", O_RDONLY);
        dup2(fd, 0);
        int lf = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (lf >= 0) {
            dup2(lf, 1);
            dup2(lf, 2);
        }
        if (fork() > 0)
            _exit(0);   /* double fork: fully detach */
        if (g_home_override)
            execl(exe, exe, "--home", g_home_override, "daemon", (char *)NULL);
        else
            execl(exe, exe, "daemon", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 50; i++) {
        usleep(100 * 1000);
        if (ipc_ping())
            return true;
    }
    fprintf(stderr, "beam: the daemon did not start; see %s\n", logpath);
    return false;
}

/* Calls the daemon; prints errors. Returns NULL on failure. */
static cJSON *call(const char *cmd, cJSON *args)
{
    if (!start_daemon()) {
        fprintf(stderr, "beam: cannot reach the daemon\n");
        return NULL;
    }
    cJSON *r = ipc_call(cmd, args);
    if (!r) {
        fprintf(stderr, "beam: no reply from the daemon\n");
        return NULL;
    }
    const cJSON *e = cJSON_GetObjectItem(r, "error");
    if (cJSON_IsString(e)) {
        fprintf(stderr, "beam: %s\n", e->valuestring);
        cJSON_Delete(r);
        return NULL;
    }
    return r;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static double jnum(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static cJSON *id_args(const char *id)
{
    cJSON *a = cJSON_CreateObject();
    cJSON_AddNumberToObject(a, "id", atoi(id));
    return a;
}

/* ---- progress display ---- */

static int term_width(void)
{
    struct winsize ws;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20)
        return ws.ws_col;
    return 80;
}

static void fmt_eta(double s, char *out, size_t n)
{
    if (s < 0)
        snprintf(out, n, "--:--");
    else if (s >= 3600)
        snprintf(out, n, "%d:%02d:%02d", (int)s / 3600, ((int)s / 60) % 60, (int)s % 60);
    else
        snprintf(out, n, "%d:%02d", (int)s / 60, (int)s % 60);
}

static void draw_progress(const cJSON *t)
{
    bool tty = isatty(STDERR_FILENO);
    double total = jnum(t, "total"), done = jnum(t, "done"), rate = jnum(t, "rate");
    const char *state = jstr(t, "state");
    bool bench = cJSON_IsTrue(cJSON_GetObjectItem(t, "bench"));
    char d[32], tot[32], r[32], eta[32];
    human_bytes(done, d, sizeof d);
    human_bytes(total, tot, sizeof tot);
    human_rate(rate, r, sizeof r);
    fmt_eta(jnum(t, "eta"), eta, sizeof eta);
    double frac = total > 0 ? done / total : 0;
    if (bench) {
        double secs = jnum(t, "elapsed");
        frac = MIN(1.0, secs / MAX(1, jnum(t, "eta") + secs));
    }

    char tail[160];
    if (!strcmp(state, "active") || !strcmp(state, "finishing")) {
        double ratio = jnum(t, "ratio");
        char rs[24] = "";
        if (ratio > 0 && ratio < 0.97)
            snprintf(rs, sizeof rs, "  zstd %.0f%%", ratio * 100);
        if (bench)
            snprintf(tail, sizeof tail, " %s  %s  %s", d, r, eta);
        else
            snprintf(tail, sizeof tail, " %5.1f%%  %s / %s  %s  ETA %s%s", frac * 100, d, tot, r, eta, rs);
    } else {
        snprintf(tail, sizeof tail, " %s…", state);
    }
    if (!tty) {
        fprintf(stderr, "%s\n", tail + 1);
        return;
    }
    int width = term_width() - (int)strlen(tail) - 3;
    width = MAX(10, MIN(width, 40));
    int fill = (int)(frac * width);
    fprintf(stderr, "\r\033[K[");
    for (int i = 0; i < width; i++)
        fputs(i < fill ? "█" : "░", stderr);
    fprintf(stderr, "]%s", tail);
    fflush(stderr);
}

static bool is_final(const char *state)
{
    return !strcmp(state, "done") || !strcmp(state, "failed") || !strcmp(state, "cancelled") ||
           !strcmp(state, "rejected");
}

/* Follows a transfer until it ends. Ctrl-C cancels it. Returns exit code. */
static int watch(int id)
{
    signal(SIGINT, on_sigint);
    char ids[16];
    snprintf(ids, sizeof ids, "%d", id);
    double last_draw = 0;
    bool tty = isatty(STDERR_FILENO);
    for (;;) {
        if (g_interrupted) {
            cJSON *a = id_args(ids);
            cJSON_Delete(ipc_call("cancel", a));
            cJSON_Delete(a);
            g_interrupted = 0;
            fprintf(stderr, "\ncancelling…\n");
        }
        cJSON *a = id_args(ids);
        cJSON *t = ipc_call("transfer", a);
        cJSON_Delete(a);
        if (!t) {
            fprintf(stderr, "\nbeam: lost contact with the daemon\n");
            return 1;
        }
        const char *state = jstr(t, "state");
        if (is_final(state)) {
            if (tty)
                fprintf(stderr, "\r\033[K");
            int rc = 0;
            char sz[32], r[32];
            human_bytes(jnum(t, "total"), sz, sizeof sz);
            human_rate(jnum(t, "rate"), r, sizeof r);
            bool bench = cJSON_IsTrue(cJSON_GetObjectItem(t, "bench"));
            if (!strcmp(state, "done") && bench) {
                char d[32];
                human_bytes(jnum(t, "done"), d, sizeof d);
                printf("link to %s: %s  (%s in %.1fs over %d streams)\n", jstr(t, "peer"), r, d,
                       jnum(t, "elapsed"), (int)jnum(t, "streams"));
            } else if (!strcmp(state, "done")) {
                double ratio = jnum(t, "ratio");
                printf("✓ %s %s (%s, %d files) in %.1fs — %s avg%s%s\n",
                       !strcmp(jstr(t, "direction"), "out") ? "sent to" : "received from", jstr(t, "peer"), sz,
                       (int)jnum(t, "files"), jnum(t, "elapsed"), r,
                       cJSON_IsTrue(cJSON_GetObjectItem(t, "resumed")) ? ", resumed" : "",
                       ratio < 0.97 && ratio > 0 ? ", compressed" : "");
                if (!strcmp(jstr(t, "direction"), "in"))
                    printf("  saved to %s\n", jstr(t, "dest"));
            } else {
                fprintf(stderr, "✗ %s: %s\n", state, jstr(t, "error"));
                rc = 1;
            }
            cJSON_Delete(t);
            return rc;
        }
        if (now_sec() - last_draw > (tty ? 0.2 : 2.0)) {
            draw_progress(t);
            last_draw = now_sec();
        }
        cJSON_Delete(t);
        usleep(150 * 1000);
    }
}

/* ---- commands ---- */

static int cmd_send(int argc, char **argv, bool bench)
{
    static const struct option opts[] = {
        { "to", required_argument, 0, 't' },     { "streams", required_argument, 0, 's' },
        { "compress", required_argument, 0, 'c' }, { "code", required_argument, 0, 'k' },
        { "secs", required_argument, 0, 'S' },   { "detach", no_argument, 0, 'd' },
        { 0, 0, 0, 0 },
    };
    cJSON *a = cJSON_CreateObject();
    bool detach = false;
    int c;
    optind = 1;
    while ((c = getopt_long(argc, argv, "t:s:c:k:S:d", opts, NULL)) != -1) {
        switch (c) {
        case 't': cJSON_AddStringToObject(a, "to", optarg); break;
        case 's': cJSON_AddNumberToObject(a, "streams", atoi(optarg)); break;
        case 'c': cJSON_AddStringToObject(a, "compress", optarg); break;
        case 'k': cJSON_AddStringToObject(a, "code", optarg); break;
        case 'S': cJSON_AddNumberToObject(a, "secs", atoi(optarg)); break;
        case 'd': detach = true; break;
        default: cJSON_Delete(a); return 2;
        }
    }
    if (bench) {
        if (optind < argc && !cJSON_GetObjectItem(a, "to"))
            cJSON_AddStringToObject(a, "to", argv[optind]);
    } else {
        cJSON *paths = cJSON_AddArrayToObject(a, "paths");
        for (int i = optind; i < argc; i++) {
            char *real = realpath(argv[i], NULL);
            if (!real) {
                fprintf(stderr, "beam: %s: %s\n", argv[i], strerror(errno));
                cJSON_Delete(a);
                return 1;
            }
            /* Keep the name the user typed for symlinked top-level paths. */
            struct stat st;
            if (lstat(argv[i], &st) == 0 && S_ISLNK(st.st_mode)) {
                free(real);
                char cwd[PATH_MAX];
                if (argv[i][0] != '/' && getcwd(cwd, sizeof cwd) && asprintf(&real, "%s/%s", cwd, argv[i]) < 0)
                    abort();
                if (!real || argv[i][0] == '/')
                    real = xstrdup(argv[i]);
            }
            cJSON_AddItemToArray(paths, cJSON_CreateString(real));
            free(real);
        }
        if (cJSON_GetArraySize(paths) == 0) {
            fprintf(stderr, "usage: beam send <path>... --to <peer>\n");
            cJSON_Delete(a);
            return 2;
        }
    }
    if (!cJSON_GetObjectItem(a, "to")) {
        fprintf(stderr, "beam: say where to send with --to <peer>  (see `beam peers`)\n");
        cJSON_Delete(a);
        return 2;
    }
    cJSON *r = call(bench ? "bench" : "send", a);
    cJSON_Delete(a);
    if (!r)
        return 1;
    int id = (int)jnum(r, "id");
    cJSON_Delete(r);
    if (detach) {
        printf("transfer %d started — follow it with `beam watch %d`\n", id, id);
        return 0;
    }
    return watch(id);
}

static int cmd_peers(void)
{
    cJSON *r = call("peers", NULL);
    if (!r)
        return 1;
    const cJSON *paired = cJSON_GetObjectItem(r, "paired"), *disc = cJSON_GetObjectItem(r, "discovered"), *p;
    printf("Paired:\n");
    if (!cJSON_GetArraySize(paired))
        printf("  (none — pair with `beam pair`)\n");
    cJSON_ArrayForEach(p, paired) {
        printf("  %-20s %s  %s:%d  %s\n", jstr(p, "name"), jstr(p, "fp"), jstr(p, "host"), (int)jnum(p, "port"),
               cJSON_IsTrue(cJSON_GetObjectItem(p, "online")) ? "online" : "not seen");
    }
    printf("\nOn this network:\n");
    if (!cJSON_GetArraySize(disc))
        printf("  (nothing discovered yet — is beam running on the other machine, and UDP %d open?)\n",
               DISCOVERY_PORT);
    cJSON_ArrayForEach(p, disc) {
        char link[48];
        int sp = (int)jnum(p, "speed");
        if (sp >= 1000)
            snprintf(link, sizeof link, "%s %g Gbit/s", jstr(p, "iface"), sp / 1000.0);
        else if (sp > 0)
            snprintf(link, sizeof link, "%s %d Mbit/s", jstr(p, "iface"), sp);
        else
            snprintf(link, sizeof link, "%s%s", jstr(p, "iface"),
                     cJSON_IsTrue(cJSON_GetObjectItem(p, "wireless")) ? " (Wi-Fi)" : "");
        printf("  %-20s %s  %s:%d  via %s%s\n", jstr(p, "name"), jstr(p, "fp"), jstr(p, "addr"),
               (int)jnum(p, "port"), link, cJSON_IsTrue(cJSON_GetObjectItem(p, "paired")) ? "  [paired]" : "");
    }
    cJSON_Delete(r);
    return 0;
}

static int cmd_pair(int argc, char **argv)
{
    if (argc >= 3) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "to", argv[1]);
        cJSON_AddStringToObject(a, "code", argv[2]);
        cJSON *r = call("pair_join", a);
        cJSON_Delete(a);
        if (!r)
            return 1;
        printf("✓ paired with %s (%s)\n", jstr(r, "name"), jstr(r, "fp"));
        cJSON_Delete(r);
        return 0;
    }
    cJSON *before = call("peers", NULL);
    if (!before)
        return 1;
    int n0 = cJSON_GetArraySize(cJSON_GetObjectItem(before, "paired"));
    cJSON_Delete(before);
    cJSON *r = call("pair_start", NULL);
    if (!r)
        return 1;
    printf("Pairing code:  %s\n\nOn the other machine run:\n  beam pair %s %s\n\n(valid for %d minutes, one attempt)\n",
           jstr(r, "code"), jstr(r, "name"), jstr(r, "code"), (int)jnum(r, "ttl") / 60);
    fflush(stdout);
    double until = now_sec() + jnum(r, "ttl");
    cJSON_Delete(r);
    signal(SIGINT, on_sigint);
    while (!g_interrupted && now_sec() < until) {
        sleep(1);
        cJSON *p = ipc_call("peers", NULL);
        if (!p)
            return 1;
        const cJSON *paired = cJSON_GetObjectItem(p, "paired");
        if (cJSON_GetArraySize(paired) > n0) {
            /* peers are appended, so the newest is last */
            const cJSON *np = cJSON_GetArrayItem(paired, cJSON_GetArraySize(paired) - 1);
            printf("✓ paired with %s (%s)\n", jstr(np, "name"), jstr(np, "fp"));
            cJSON_Delete(p);
            return 0;
        }
        cJSON *st = ipc_call("state", NULL);
        int ttl = st ? (int)jnum(st, "pair_ttl") : 0;
        cJSON_Delete(st);
        cJSON_Delete(p);
        if (ttl == 0) {
            fprintf(stderr, "pairing attempt failed (wrong code?) — run `beam pair` again\n");
            return 1;
        }
    }
    return 1;
}

static int cmd_recv(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    cJSON *r = call("recv_code", NULL);
    if (!r)
        return 1;
    printf("One-shot code:  %s\n\nOn the sending machine run:\n  beam send <files> --to %s --code %s\n\n"
           "Waiting for the transfer… (Ctrl-C to stop waiting)\n",
           jstr(r, "code"), jstr(r, "name"), jstr(r, "code"));
    fflush(stdout);
    double until = now_sec() + jnum(r, "ttl");
    cJSON_Delete(r);

    /* Wait for a new incoming one-shot transfer, then follow it. */
    cJSON *t0 = ipc_call("transfers", NULL);
    int max_id = 0;
    const cJSON *t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItem(t0, "transfers")) max_id = MAX(max_id, (int)jnum(t, "id"));
    cJSON_Delete(t0);
    signal(SIGINT, on_sigint);
    while (!g_interrupted && now_sec() < until) {
        usleep(300 * 1000);
        cJSON *l = ipc_call("transfers", NULL);
        if (!l)
            return 1;
        cJSON_ArrayForEach(t, cJSON_GetObjectItem(l, "transfers")) {
            if ((int)jnum(t, "id") > max_id && !strcmp(jstr(t, "direction"), "in")) {
                int id = (int)jnum(t, "id");
                printf("Receiving %s from %s\n", jstr(t, "title"), jstr(t, "peer"));
                cJSON_Delete(l);
                return watch(id);
            }
        }
        cJSON_Delete(l);
    }
    return 1;
}

static void print_transfer_row(const cJSON *t)
{
    char sz[32], r[32];
    human_bytes(jnum(t, "total"), sz, sizeof sz);
    human_rate(jnum(t, "rate"), r, sizeof r);
    double pct = jnum(t, "total") > 0 ? 100 * jnum(t, "done") / jnum(t, "total") : 0;
    time_t when = (time_t)(jnum(t, "started") / 1000);
    char ts[32];
    strftime(ts, sizeof ts, "%m-%d %H:%M", localtime(&when));
    printf("%4d  %s  %-3s %-16.16s %-28.28s %9s %5.1f%%  %-14s %-10s %s\n", (int)jnum(t, "id"), ts,
           !strcmp(jstr(t, "direction"), "out") ? "→" : "←", jstr(t, "peer"), jstr(t, "title"), sz, pct, r,
           jstr(t, "state"), jstr(t, "error"));
}

static int cmd_list(const char *which)
{
    bool hist = !strcmp(which, "history");
    cJSON *r = call(hist ? "history" : "transfers", NULL);
    if (!r)
        return 1;
    const cJSON *arr = cJSON_GetObjectItem(r, hist ? "history" : "transfers"), *t;
    if (!cJSON_GetArraySize(arr))
        printf("no transfers yet\n");
    else
        printf("%4s  %-11s %-3s %-16s %-28s %9s %6s  %-14s %s\n", "id", "started", "", "peer", "what", "size", "",
               "speed", "state");
    cJSON_ArrayForEach(t, arr) print_transfer_row(t);
    cJSON_Delete(r);
    return 0;
}

static int cmd_status(void)
{
    cJSON *r = call("info", NULL);
    if (!r)
        return 1;
    printf("beam %s — daemon running\n", jstr(r, "version"));
    printf("  device       %s (%s)\n", jstr(r, "name"), jstr(r, "fp"));
    printf("  peer port    %d (TCP), discovery UDP %d\n", (int)jnum(r, "peer_port"), DISCOVERY_PORT);
    printf("  web UI       http://127.0.0.1:%d  (open with `beam ui`)\n", (int)jnum(r, "web_port"));
    printf("  downloads    %s\n", jstr(r, "download_dir"));
    printf("  auto-accept  %s from paired peers\n", cJSON_IsTrue(cJSON_GetObjectItem(r, "auto_accept")) ? "on" : "off");
    int comp = (int)jnum(r, "compress");
    printf("  streams      %d    compression %s    cipher %s\n", (int)jnum(r, "streams"),
           comp == COMPRESS_AUTO ? "auto" : comp == COMPRESS_OFF ? "off" : "fixed level", jstr(r, "cipher"));
    const cJSON *i;
    cJSON_ArrayForEach(i, cJSON_GetObjectItem(r, "interfaces")) {
        int sp = (int)jnum(i, "speed");
        char link[32] = "";
        if (sp > 0)
            snprintf(link, sizeof link, "%d Mbit/s", sp);
        else if (cJSON_IsTrue(cJSON_GetObjectItem(i, "wireless")))
            snprintf(link, sizeof link, "Wi-Fi");
        printf("  interface    %-10s %-15s %s\n", jstr(i, "iface"), jstr(i, "addr"), link);
    }
    cJSON_Delete(r);
    return 0;
}

static int cmd_config(int argc, char **argv)
{
    if (argc < 3)
        return cmd_status();
    cJSON *a = cJSON_CreateObject();
    const char *k = argv[1], *v = argv[2];
    if (!strcmp(k, "auto_accept"))
        cJSON_AddBoolToObject(a, k, !strcmp(v, "on") || !strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "yes"));
    else if (!strcmp(k, "streams"))
        cJSON_AddNumberToObject(a, k, atoi(v));
    else if (!strcmp(k, "download_dir")) {
        char *real = realpath(v, NULL);
        cJSON_AddStringToObject(a, k, real ? real : v);
        free(real);
    } else if (!strcmp(k, "name") || !strcmp(k, "compress"))
        cJSON_AddStringToObject(a, k, v);
    else {
        fprintf(stderr, "beam: unknown setting '%s'\n", k);
        cJSON_Delete(a);
        return 2;
    }
    cJSON *r = call("config_set", a);
    cJSON_Delete(a);
    if (!r)
        return 1;
    cJSON_Delete(r);
    return cmd_status();
}

static int cmd_ui(void)
{
    cJSON *r = call("info", NULL);
    if (!r)
        return 1;
    char url[256];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/#token=%s", (int)jnum(r, "web_port"), g_cfg.web_token);
    cJSON_Delete(r);
    printf("%s\n", url);
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        dup2(fd, 1);
        dup2(fd, 2);
        execlp("xdg-open", "xdg-open", url, (char *)NULL);
        _exit(127);
    }
    return 0;
}

static int cmd_daemon(int argc, char **argv)
{
    static const struct option opts[] = {
        { "port", required_argument, 0, 'p' }, { "web", required_argument, 0, 'w' },
        { "dir", required_argument, 0, 'd' },  { "name", required_argument, 0, 'n' },
        { "verbose", no_argument, 0, 'v' },    { 0, 0, 0, 0 },
    };
    int c;
    bool changed = false;
    optind = 1;
    while ((c = getopt_long(argc, argv, "p:w:d:n:v", opts, NULL)) != -1) {
        switch (c) {
        case 'p': g_cfg.peer_port = atoi(optarg); changed = true; break;
        case 'w': g_cfg.web_port = atoi(optarg); changed = true; break;
        case 'd': {
            char *real = realpath(optarg, NULL);
            mkdir_p(real ? real : optarg, 0755);
            snprintf(g_cfg.download_dir, sizeof g_cfg.download_dir, "%s", real ? real : optarg);
            free(real);
            changed = true;
            break;
        }
        case 'n': snprintf(g_cfg.name, sizeof g_cfg.name, "%s", optarg); changed = true; break;
        case 'v': g_log_level = LOG_DEBUG; break;
        default: return 2;
        }
    }
    if (changed)
        config_save();
    return daemon_main();
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--home") && i + 1 < argc) {
            g_home_override = argv[++i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else if (!strcmp(argv[i], "--version")) {
            puts(BEAM_VERSION);
            return 0;
        } else {
            fprintf(stderr, "beam: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (i >= argc) {
        usage();
        return 2;
    }
    if (crypt_init() < 0) {
        fprintf(stderr, "beam: cannot initialise libsodium\n");
        return 1;
    }
    if (config_load(g_home_override) < 0) {
        fprintf(stderr, "beam: cannot load configuration from %s\n", g_cfg.home);
        return 1;
    }
    const char *cmd = argv[i];
    int sub_argc = argc - i;
    char **sub_argv = argv + i;

    if (!strcmp(cmd, "daemon"))
        return cmd_daemon(sub_argc, sub_argv);
    if (!strcmp(cmd, "send"))
        return cmd_send(sub_argc, sub_argv, false);
    if (!strcmp(cmd, "bench"))
        return cmd_send(sub_argc, sub_argv, true);
    if (!strcmp(cmd, "recv") || !strcmp(cmd, "receive"))
        return cmd_recv(sub_argc, sub_argv);
    if (!strcmp(cmd, "pair"))
        return cmd_pair(sub_argc, sub_argv);
    if (!strcmp(cmd, "peers"))
        return cmd_peers();
    if (!strcmp(cmd, "ls") || !strcmp(cmd, "transfers") || !strcmp(cmd, "history"))
        return cmd_list(cmd);
    if (!strcmp(cmd, "status") || !strcmp(cmd, "info"))
        return cmd_status();
    if (!strcmp(cmd, "config"))
        return cmd_config(sub_argc, sub_argv);
    if (!strcmp(cmd, "ui"))
        return cmd_ui();
    if (!strcmp(cmd, "stop")) {
        if (!ipc_ping()) {
            printf("daemon is not running\n");
            return 0;
        }
        cJSON_Delete(ipc_call("shutdown", NULL));
        for (int k = 0; k < 30 && ipc_ping(); k++)
            usleep(100 * 1000);
        printf(ipc_ping() ? "daemon did not stop\n" : "daemon stopped\n");
        return 0;
    }
    if (!strcmp(cmd, "unpair") && sub_argc >= 2) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "peer", sub_argv[1]);
        cJSON *r = call("unpair", a);
        cJSON_Delete(a);
        if (!r)
            return 1;
        cJSON_Delete(r);
        printf("forgot %s\n", sub_argv[1]);
        return 0;
    }
    if ((!strcmp(cmd, "watch") || !strcmp(cmd, "accept") || !strcmp(cmd, "reject") || !strcmp(cmd, "cancel")) &&
        sub_argc >= 2) {
        if (!strcmp(cmd, "watch")) {
            if (!start_daemon())
                return 1;
            return watch(atoi(sub_argv[1]));
        }
        cJSON *a = id_args(sub_argv[1]);
        cJSON *r = call(cmd, a);
        cJSON_Delete(a);
        if (!r)
            return 1;
        cJSON_Delete(r);
        if (!strcmp(cmd, "accept"))
            return watch(atoi(sub_argv[1]));
        printf("%s: ok\n", cmd);
        return 0;
    }
    usage();
    return 2;
}
