#include "receiver.h"
#include "proto.h"
#include "manifest.h"
#include "journal.h"
#include "compress.h"
#include "transfer.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <xxhash.h>

#define DECISION_TIMEOUT 600   /* seconds to wait for the user to accept */
#define STALL_TIMEOUT    60    /* seconds without data before giving up */
#define FLUSH_EVERY      3.0   /* journal flush interval, seconds */
#define MAX_NEED         900000

typedef struct rsession {
    uint8_t id[SID_LEN];
    session_conn_t *conn;
    transfer_t *t;
    manifest_t m;
    bool bench;

    journal_t j;
    bool have_journal;
    char root[1024];
    int root_fd;              /* for syncfs */
    char **local;             /* per entry: final local path */
    _Atomic int32_t *remaining; /* per entry: chunks still missing */
    int *fds;                 /* per entry: open .beam-part (large files) */
    pthread_mutex_t fd_lock;

    pthread_mutex_t lock;
    pthread_cond_t cond;
    uint32_t recv_count[MAX_STREAMS];
    int data_fds[MAX_STREAMS];
    int n_threads;
    double last_data;
    bool closing;
    bool io_error;
    char io_err[256];
    int refs;
    struct rsession *next;
} rsession_t;

static pthread_mutex_t g_sess_lock = PTHREAD_MUTEX_INITIALIZER;
static rsession_t *g_sessions;

/* ---- session registry ---- */

static rsession_t *session_lookup(const uint8_t id[SID_LEN])
{
    pthread_mutex_lock(&g_sess_lock);
    rsession_t *r = g_sessions;
    while (r && (r->closing || sodium_memcmp(r->id, id, SID_LEN)))
        r = r->next;
    if (r)
        r->refs++;
    pthread_mutex_unlock(&g_sess_lock);
    return r;
}

static void session_unref(rsession_t *r)
{
    pthread_mutex_lock(&g_sess_lock);
    bool last = --r->refs == 0;
    if (last) {
        for (rsession_t **pp = &g_sessions; *pp; pp = &(*pp)->next) {
            if (*pp == r) {
                *pp = r->next;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_sess_lock);
    if (!last)
        return;
    for (uint32_t i = 0; r->local && i < r->m.n_ents; i++) {
        free(r->local[i]);
        if (r->fds && r->fds[i] >= 0)
            close(r->fds[i]);
    }
    free(r->local);
    free(r->fds);
    free(r->remaining);
    if (r->have_journal)
        journal_close(&r->j, false);
    if (r->root_fd >= 0)
        close(r->root_fd);
    manifest_free(&r->m);
    pthread_mutex_destroy(&r->fd_lock);
    pthread_mutex_destroy(&r->lock);
    pthread_cond_destroy(&r->cond);
    free(r);
}

/* A sender that reconnects quickly may race its own stale session. */
static bool transfer_id_active(const char *journal_path)
{
    bool hit = false;
    pthread_mutex_lock(&g_sess_lock);
    for (rsession_t *r = g_sessions; r && !hit; r = r->next)
        hit = !r->closing && r->have_journal && !strcmp(r->j.path, journal_path);
    pthread_mutex_unlock(&g_sess_lock);
    return hit;
}

static void io_fail(rsession_t *r, const char *what, const char *path)
{
    pthread_mutex_lock(&r->lock);
    if (!r->io_error) {
        r->io_error = true;
        snprintf(r->io_err, sizeof r->io_err, "%s %s: %s", what, path, strerror(errno));
    }
    pthread_cond_broadcast(&r->cond);
    pthread_mutex_unlock(&r->lock);
}

/* ---- file handling ---- */

static void part_path(const char *final, char *out, size_t n)
{
    snprintf(out, n, "%s.beam-part", final);
}

static int open_part(const char *final, int flags)
{
    char part[4200];
    part_path(final, part, sizeof part);
    int fd = open(part, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | flags, 0600);
    if (fd < 0 && errno == ENOENT) {
        char dir[4200];
        snprintf(dir, sizeof dir, "%s", final);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = 0;
            mkdir_p(dir, 0755);
        }
        fd = open(part, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | flags, 0600);
    }
    return fd;
}

static void apply_attrs(int fd, const entry_t *e)
{
    fchmod(fd, e->mode & 07777);
    struct timespec ts[2] = {
        { .tv_nsec = UTIME_OMIT },
        { .tv_sec = e->mtime_ns / 1000000000LL, .tv_nsec = e->mtime_ns % 1000000000LL },
    };
    futimens(fd, ts);
}

/* Applies metadata, closes, and moves the .beam-part into place. */
static int finalize_fd(rsession_t *r, uint32_t ei, int fd)
{
    const entry_t *e = &r->m.ents[ei];
    apply_attrs(fd, e);
    close(fd);
    char part[4200];
    part_path(r->local[ei], part, sizeof part);
    if (rename(part, r->local[ei]) < 0) {
        io_fail(r, "cannot rename", part);
        return -1;
    }
    return 0;
}

static int finalize_cached(rsession_t *r, uint32_t ei)
{
    pthread_mutex_lock(&r->fd_lock);
    int fd = r->fds[ei];
    r->fds[ei] = -1;
    pthread_mutex_unlock(&r->fd_lock);
    if (fd < 0) {
        /* Resumed file whose chunks were all written earlier. */
        char part[4200];
        part_path(r->local[ei], part, sizeof part);
        fd = open(part, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0)
            return errno == ENOENT ? 0 : (io_fail(r, "cannot open", part), -1);
    }
    return finalize_fd(r, ei, fd);
}

static int get_fd(rsession_t *r, uint32_t ei)
{
    pthread_mutex_lock(&r->fd_lock);
    int fd = r->fds[ei];
    if (fd < 0) {
        fd = open_part(r->local[ei], 0);
        if (fd >= 0) {
            struct stat st;
            /* Reserve space up front: avoids fragmentation and fails early
             * when the disk is full. */
            if (fstat(fd, &st) == 0 && (uint64_t)st.st_size < r->m.ents[ei].size &&
                fallocate(fd, 0, 0, (off_t)r->m.ents[ei].size) < 0 && errno == ENOSPC) {
                close(fd);
                fd = -1;
                errno = ENOSPC;
            }
        }
        r->fds[ei] = fd;
    }
    pthread_mutex_unlock(&r->fd_lock);
    return fd;
}

static int pwrite_full(int fd, const uint8_t *p, size_t n, off_t off)
{
    while (n) {
        ssize_t w = pwrite(fd, p, n, off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
        off += w;
    }
    return 0;
}

static int write_chunk(rsession_t *r, uint32_t idx, const uint8_t *data, bool *finalize_bundle)
{
    const chunk_t *c = &r->m.chunks[idx];
    *finalize_bundle = false;
    if (c->kind == CHUNK_FILE) {
        int fd = get_fd(r, c->entry);
        if (fd < 0) {
            io_fail(r, "cannot create", r->local[c->entry]);
            return -1;
        }
        if (pwrite_full(fd, data, c->len, (off_t)c->offset) < 0) {
            io_fail(r, "cannot write", r->local[c->entry]);
            return -1;
        }
        return 0;
    }
    /* Bundle: whole small files back to back. */
    const uint8_t *p = data;
    for (uint32_t k = 0; k < c->count; k++) {
        uint32_t ei = r->m.bundle_list[c->entry + k];
        const entry_t *e = &r->m.ents[ei];
        int fd = open_part(r->local[ei], O_TRUNC);
        if (fd < 0 || write_full(fd, p, e->size) < 0) {
            io_fail(r, "cannot write", r->local[ei]);
            if (fd >= 0)
                close(fd);
            return -1;
        }
        p += e->size;
        apply_attrs(fd, e);
        close(fd);
    }
    *finalize_bundle = true;
    return 0;
}

static void finalize_bundle_files(rsession_t *r, uint32_t idx)
{
    const chunk_t *c = &r->m.chunks[idx];
    for (uint32_t k = 0; k < c->count; k++) {
        uint32_t ei = r->m.bundle_list[c->entry + k];
        char part[4200];
        part_path(r->local[ei], part, sizeof part);
        if (rename(part, r->local[ei]) < 0)
            io_fail(r, "cannot rename", part);
        atomic_store(&r->remaining[ei], 0);
    }
}

/* ---- data streams ---- */

void receiver_data(int fd, const uint8_t session_id[SID_LEN], uint16_t stream)
{
    rsession_t *r = session_lookup(session_id);
    if (!r || stream >= MAX_STREAMS) {
        uint8_t st = ST_UNKNOWN;
        write_full(fd, &st, 1);
        close(fd);
        if (r)
            session_unref(r);
        return;
    }
    uint8_t resp[1 + SID_LEN] = { ST_OK };
    randombytes_buf(resp + 1, SID_LEN);
    if (write_full(fd, resp, sizeof resp) < 0) {
        close(fd);
        session_unref(r);
        return;
    }
    /* Data streams can sit idle while the receiver verifies; the session
     * shuts them down explicitly when it ends. */
    struct timeval tv = { 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    chan_t ch;
    chan_init(&ch, fd, r->conn->cipher, r->conn->master, "data", stream, resp + 1, false);
    pthread_mutex_lock(&r->lock);
    r->data_fds[stream] = fd;
    r->n_threads++;
    pthread_mutex_unlock(&r->lock);

    void *dctx = decompressor_new();
    uint8_t *raw = r->bench ? NULL : xmalloc(r->m.chunk_size);
    transfer_t *t = r->t;

    for (;;) {
        size_t n;
        uint8_t *msg = chan_recv(&ch, &n);
        if (!msg)
            break;
        if (msg[0] == MSG_BENCH) {
            atomic_fetch_add(&t->bytes_done, n);
            atomic_fetch_add(&t->wire_bytes, n);
        } else if (msg[0] == MSG_CHUNK && n >= CHUNK_HDR_LEN && !r->bench) {
            uint32_t idx = get_u32(msg + 1);
            uint8_t flags = msg[5];
            uint32_t raw_len = get_u32(msg + 6);
            uint64_t hash = get_u64(msg + 10);
            const uint8_t *payload = msg + CHUNK_HDR_LEN;
            size_t plen = n - CHUNK_HDR_LEN;
            if (idx >= r->m.n_chunks || raw_len != r->m.chunks[idx].len) {
                LOGW("protocol error on data stream %u", stream);
                break;
            }
            const uint8_t *data = payload;
            bool ok = true;
            if (flags & CHUNK_F_ZSTD) {
                ok = decompress_chunk(dctx, payload, plen, raw, raw_len) == 0;
                data = raw;
            } else if (plen != raw_len) {
                ok = false;
            }
            if (ok && XXH3_64bits(data, raw_len) != hash)
                ok = false;
            if (!ok) {
                /* Leave the bit clear; the end-of-round check asks for it again. */
                LOGW("chunk %u failed verification, will re-request", idx);
            } else if (!journal_test(&r->j, idx)) {
                bool bundle;
                if (write_chunk(r, idx, data, &bundle) == 0 && journal_set(&r->j, idx)) {
                    const chunk_t *c = &r->m.chunks[idx];
                    if (bundle)
                        finalize_bundle_files(r, idx);
                    else if (atomic_fetch_sub(&r->remaining[c->entry], 1) == 1)
                        finalize_cached(r, c->entry);
                    atomic_fetch_add(&t->bytes_done, raw_len);
                    atomic_fetch_add(&t->wire_bytes, plen);
                }
            }
        } else {
            LOGW("unexpected message %d on data stream", msg[0]);
            break;
        }
        pthread_mutex_lock(&r->lock);
        r->recv_count[stream]++;
        r->last_data = now_sec();
        pthread_cond_broadcast(&r->cond);
        pthread_mutex_unlock(&r->lock);
    }

    free(raw);
    decompressor_free(dctx);
    chan_free(&ch);
    pthread_mutex_lock(&r->lock);
    r->data_fds[stream] = -1;
    r->n_threads--;
    pthread_cond_broadcast(&r->cond);
    pthread_mutex_unlock(&r->lock);
    close(fd);
    session_unref(r);
}

/* ---- control ---- */

static int send_simple(chan_t *c, uint8_t type, const char *text)
{
    buf_t b = { 0 };
    buf_u8(&b, type);
    if (text)
        buf_str(&b, text);
    int rc = chan_send(c, b.data, b.len);
    buf_free(&b);
    return rc;
}

/* Top-level name mapping, stored in the journal so a resumed transfer
 * writes into the same place. Format: n u32, then n × (orig str, local str). */
static void build_mapping(rsession_t *r, buf_t *meta)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < r->m.n_ents; i++)
        if (!strchr(r->m.ents[i].path, '/'))
            n++;
    buf_u32(meta, n);
    for (uint32_t i = 0; i < r->m.n_ents; i++) {
        const char *name = r->m.ents[i].path;
        if (strchr(name, '/'))
            continue;
        /* Don't clobber existing files: "photos" → "photos (1)". */
        char cand[1024], path[2100];
        snprintf(cand, sizeof cand, "%s", name);
        for (int k = 1; k < 1000; k++) {
            snprintf(path, sizeof path, "%s/%s", r->root, cand);
            char part[2200];
            part_path(path, part, sizeof part);
            struct stat st;
            if (lstat(path, &st) < 0 && lstat(part, &st) < 0)
                break;
            const char *dot = r->m.ents[i].type == ENT_FILE ? strrchr(name, '.') : NULL;
            if (dot && dot != name)
                snprintf(cand, sizeof cand, "%.*s (%d)%s", (int)(dot - name), name, k, dot);
            else
                snprintf(cand, sizeof cand, "%s (%d)", name, k);
        }
        buf_str(meta, name);
        buf_str(meta, cand);
    }
}

static int apply_mapping(rsession_t *r, const buf_t *meta)
{
    rd_t rd = { meta->data, meta->data + meta->len, false };
    uint32_t n = rd_u32(&rd);
    char **orig = xcalloc(n + 1, sizeof *orig), **loc = xcalloc(n + 1, sizeof *loc);
    for (uint32_t i = 0; i < n && !rd.err; i++) {
        orig[i] = rd_str(&rd);
        loc[i] = rd_str(&rd);
    }
    int rc = rd.err ? -1 : 0;
    r->local = xcalloc(r->m.n_ents, sizeof *r->local);
    for (uint32_t e = 0; e < r->m.n_ents && rc == 0; e++) {
        const char *p = r->m.ents[e].path;
        size_t toplen = strcspn(p, "/");
        const char *mapped = NULL;
        for (uint32_t i = 0; i < n; i++) {
            if (orig[i] && strlen(orig[i]) == toplen && !memcmp(orig[i], p, toplen)) {
                mapped = loc[i];
                break;
            }
        }
        if (!mapped || strchr(mapped, '/') || !strcmp(mapped, "..")) {
            rc = -1;
            break;
        }
        if (asprintf(&r->local[e], "%s/%s%s", r->root, mapped, p + toplen) < 0)
            abort();
    }
    for (uint32_t i = 0; i < n; i++) {
        free(orig[i]);
        free(loc[i]);
    }
    free(orig);
    free(loc);
    return rc;
}

/* Creates directories and empty files, and works out which files are
 * still incomplete (on resume, some may already be finished). */
static int prepare(rsession_t *r)
{
    manifest_t *m = &r->m;
    r->remaining = xcalloc(m->n_ents, sizeof *r->remaining);
    r->fds = xmalloc(m->n_ents * sizeof *r->fds);
    for (uint32_t i = 0; i < m->n_ents; i++)
        r->fds[i] = -1;

    for (uint32_t i = 0; i < m->n_chunks; i++) {
        if (journal_test(&r->j, i))
            continue;
        const chunk_t *c = &m->chunks[i];
        if (c->kind == CHUNK_FILE)
            r->remaining[c->entry]++;
        else
            for (uint32_t k = 0; k < c->count; k++)
                r->remaining[m->bundle_list[c->entry + k]] = 1;
    }
    for (uint32_t i = 0; i < m->n_ents; i++) {
        const entry_t *e = &m->ents[i];
        if (e->type == ENT_DIR) {
            if (mkdir_p(r->local[i], 0755) < 0) {
                io_fail(r, "cannot create directory", r->local[i]);
                return -1;
            }
        } else if (e->type == ENT_FILE && e->size == 0) {
            int fd = open_part(r->local[i], O_TRUNC);
            if (fd < 0) {
                io_fail(r, "cannot create", r->local[i]);
                return -1;
            }
            if (finalize_fd(r, i, fd) < 0)
                return -1;
        } else if (e->type == ENT_FILE && r->remaining[i] == 0) {
            if (finalize_cached(r, i) < 0)   /* finished last time; finalize if needed */
                return -1;
        }
    }
    return 0;
}

/* Symlinks, then directory modes and mtimes (deepest first, so writing
 * into a directory doesn't bump its mtime afterwards). */
static void finalize_tree(rsession_t *r)
{
    manifest_t *m = &r->m;
    for (uint32_t i = 0; i < m->n_ents; i++) {
        const entry_t *e = &m->ents[i];
        if (e->type != ENT_LINK)
            continue;
        unlink(r->local[i]);
        if (symlink(e->target, r->local[i]) < 0) {
            LOGW("cannot create symlink %s: %s", r->local[i], strerror(errno));
            continue;
        }
        struct timespec ts[2] = { { .tv_nsec = UTIME_OMIT },
                                  { .tv_sec = e->mtime_ns / 1000000000LL, .tv_nsec = e->mtime_ns % 1000000000LL } };
        utimensat(AT_FDCWD, r->local[i], ts, AT_SYMLINK_NOFOLLOW);
    }
    for (uint32_t i = m->n_ents; i-- > 0;) {
        const entry_t *e = &m->ents[i];
        if (e->type != ENT_DIR)
            continue;
        chmod(r->local[i], (e->mode & 07777) | 0700);
        struct timespec ts[2] = { { .tv_nsec = UTIME_OMIT },
                                  { .tv_sec = e->mtime_ns / 1000000000LL, .tv_nsec = e->mtime_ns % 1000000000LL } };
        utimensat(AT_FDCWD, r->local[i], ts, 0);
    }
}

/* Waits until every data stream has delivered what the sender says it
 * sent. Returns 0, or -1 on stall / error / cancel. */
static int wait_for_streams(rsession_t *r, const uint32_t *counts, uint16_t n)
{
    pthread_mutex_lock(&r->lock);
    r->last_data = now_sec();
    int rc = 0;
    for (;;) {
        bool all = true;
        for (uint16_t i = 0; i < n && i < MAX_STREAMS; i++)
            if (r->recv_count[i] < counts[i])
                all = false;
        if (all)
            break;
        if (r->io_error || atomic_load(&r->t->cancel) || now_sec() - r->last_data > STALL_TIMEOUT) {
            rc = -1;
            break;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&r->cond, &r->lock, &ts);
    }
    pthread_mutex_unlock(&r->lock);
    return rc;
}

static bool wait_decision(rsession_t *r)
{
    transfer_t *t = r->t;
    double deadline = now_sec() + DECISION_TIMEOUT;
    pthread_mutex_lock(&g_xfer_lock);
    while (t->decision == 0 && !atomic_load(&t->cancel) && now_sec() < deadline) {
        pthread_mutex_unlock(&g_xfer_lock);
        /* The sender may give up while we wait. */
        struct pollfd pfd = { .fd = r->conn->fd, .events = POLLIN };
        if (poll(&pfd, 1, 0) == 1) {
            pthread_mutex_lock(&g_xfer_lock);
            break;
        }
        pthread_mutex_lock(&g_xfer_lock);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 500 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&t->cond, &g_xfer_lock, &ts);
    }
    bool accepted = t->decision > 0;
    pthread_mutex_unlock(&g_xfer_lock);
    return accepted;
}

static void run_transfer(rsession_t *r);

void receiver_run(session_conn_t *s)
{
    /* The control channel may be quiet for a long time (big transfers). */
    struct timeval tv = { 0 };
    setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    size_t n;
    uint8_t *msg = chan_recv(&s->ctrl, &n);
    if (!msg)
        return;
    if (msg[0] == MSG_PING) {
        send_simple(&s->ctrl, MSG_PING, NULL);
        return;
    }
    if (msg[0] != MSG_OFFER)
        return;

    rd_t rd = { msg + 1, msg + n, false };
    uint8_t flags = rd_u8(&rd);
    int8_t compress = (int8_t)rd_u8(&rd);
    uint8_t streams = rd_u8(&rd);
    uint32_t chunk_size = rd_u32(&rd);
    uint64_t total = rd_u64(&rd);
    uint32_t bench_secs = rd_u32(&rd);
    char *title = rd_str(&rd);
    uint32_t mlen = rd_u32(&rd);
    const uint8_t *mdata = rd_bytes(&rd, mlen);
    (void)compress;
    (void)total;
    if (rd.err || chunk_size < 64 * 1024 || chunk_size > CHUNK_SIZE || streams == 0 || streams > MAX_STREAMS) {
        send_simple(&s->ctrl, MSG_ERROR, "malformed offer");
        free(title);
        return;
    }

    rsession_t *r = xcalloc(1, sizeof *r);
    r->conn = s;
    r->root_fd = -1;
    r->refs = 1;
    r->bench = flags & OFFER_F_BENCH;
    pthread_mutex_init(&r->lock, NULL);
    pthread_mutex_init(&r->fd_lock, NULL);
    pthread_cond_init(&r->cond, NULL);
    for (int i = 0; i < MAX_STREAMS; i++)
        r->data_fds[i] = -1;
    randombytes_buf(r->id, SID_LEN);

    char err[256] = "";
    if (!r->bench && manifest_parse(&r->m, mdata, mlen, err, sizeof err) < 0) {
        send_simple(&s->ctrl, MSG_ERROR, err);
        free(title);
        session_unref(r);
        return;
    }
    manifest_layout(&r->m, chunk_size);

    transfer_t *t = transfer_new(false);
    r->t = t;
    t->bench = r->bench;
    t->bench_secs = (int)bench_secs;
    t->one_shot = !s->paired;
    t->streams = streams;
    t->total_bytes = r->m.total_bytes;
    t->n_files = r->m.n_files;
    t->n_chunks = r->m.n_chunks;
    snprintf(t->peer_name, sizeof t->peer_name, "%s", s->peer_name);
    snprintf(t->peer_fp, sizeof t->peer_fp, "%s", s->peer_fp);
    snprintf(t->title, sizeof t->title, "%s", r->bench ? "link benchmark" : title);
    snprintf(t->dest, sizeof t->dest, "%s", g_cfg.download_dir);
    free(title);

    /* Code transfers were explicitly armed by the user; paired peers are
     * trusted when auto-accept is on; benchmarks write nothing to disk. */
    if (r->bench || !s->paired || g_cfg.auto_accept) {
        t->decision = 1;
    } else {
        LOGI("incoming transfer %d from %s: %s — waiting for `beam accept %d`", t->id, t->peer_name, t->title,
             t->id);
    }
    if (!wait_decision(r)) {
        send_simple(&s->ctrl, MSG_REJECT, "declined by receiver");
        if (t->decision < 0 || atomic_load(&t->cancel))
            transfer_fail(t, TS_REJECTED, "declined");
        else
            transfer_fail(t, TS_FAILED, "not accepted in time, or the sender gave up");
        transfer_put(t);
        session_unref(r);
        return;
    }
    run_transfer(r);
    transfer_put(t);
    session_unref(r);
}

/* The journal only says what was written; the files may have been moved
 * or deleted since. Forget chunks whose bytes are no longer on disk. */
static void verify_resume(rsession_t *r)
{
    uint8_t *ok = xcalloc(r->m.n_ents, 1);   /* 0 = unchecked, 1 = present, 2 = gone */
    for (uint32_t e = 0; e < r->m.n_ents; e++) {
        if (r->m.ents[e].type != ENT_FILE)
            continue;
        char part[4200];
        struct stat st;
        part_path(r->local[e], part, sizeof part);
        uint64_t want = r->m.ents[e].size;
        bool present = (stat(part, &st) == 0 && S_ISREG(st.st_mode) && (uint64_t)st.st_size >= want) ||
                       (lstat(r->local[e], &st) == 0 && S_ISREG(st.st_mode) && (uint64_t)st.st_size == want);
        ok[e] = present ? 1 : 2;
    }
    uint32_t dropped = 0;
    for (uint32_t i = 0; i < r->m.n_chunks; i++) {
        if (!journal_test(&r->j, i))
            continue;
        const chunk_t *c = &r->m.chunks[i];
        bool good = true;
        if (c->kind == CHUNK_FILE)
            good = ok[c->entry] == 1;
        else
            for (uint32_t k = 0; k < c->count && good; k++)
                good = ok[r->m.bundle_list[c->entry + k]] == 1;
        if (!good) {
            journal_clear(&r->j, i);
            dropped++;
        }
    }
    if (dropped)
        LOGW("transfer %d: %u chunk(s) recorded in the journal are missing on disk; re-requesting", r->t->id, dropped);
    free(ok);
}

static int open_journal(rsession_t *r, char *err, size_t errn)
{
    snprintf(r->root, sizeof r->root, "%s", g_cfg.download_dir);
    char jdir[1200], jpath[1300], idhex[33];
    snprintf(jdir, sizeof jdir, "%s/.beam", r->root);
    if (mkdir_p(jdir, 0700) < 0) {
        snprintf(err, errn, "cannot create %s: %s", jdir, strerror(errno));
        return -1;
    }
    r->root_fd = open(r->root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    /* Same files from the same sender ⇒ same id ⇒ resume. */
    buf_t mb = { 0 };
    manifest_serialize(&r->m, &mb);
    uint8_t id[16], cs[4];
    put_u32(cs, r->m.chunk_size);
    crypto_generichash_state st;
    crypto_generichash_init(&st, NULL, 0, sizeof id);
    crypto_generichash_update(&st, mb.data, mb.len);
    crypto_generichash_update(&st, r->conn->peer_pk, KEY_LEN);
    crypto_generichash_update(&st, cs, 4);
    crypto_generichash_final(&st, id, sizeof id);
    buf_free(&mb);
    hex_encode(id, sizeof id, idhex);
    snprintf(jpath, sizeof jpath, "%s/%s.journal", jdir, idhex);
    if (transfer_id_active(jpath)) {
        snprintf(err, errn, "this transfer is already running");
        return -1;
    }

    buf_t meta = { 0 };
    build_mapping(r, &meta);
    bool resumed;
    if (journal_open(&r->j, jpath, r->m.n_chunks, &meta, &resumed) < 0) {
        snprintf(err, errn, "cannot open journal %s: %s", jpath, strerror(errno));
        buf_free(&meta);
        return -1;
    }
    r->have_journal = true;
    int rc = apply_mapping(r, &meta);
    buf_free(&meta);
    if (rc < 0) {
        snprintf(err, errn, "corrupt journal");
        return -1;
    }
    if (resumed)
        verify_resume(r);
    r->t->resumed = resumed && journal_done(&r->j) > 0;
    if (r->t->resumed) {
        uint64_t have = 0;
        for (uint32_t i = 0; i < r->m.n_chunks; i++)
            if (journal_test(&r->j, i))
                have += r->m.chunks[i].len;
        r->t->skipped_bytes = have;
        atomic_store(&r->t->bytes_done, have);
        LOGI("resuming transfer %d: %u/%u chunks already here", r->t->id, journal_done(&r->j), r->m.n_chunks);
    }
    /* Show where the files land, e.g. ~/Downloads/beam/photos */
    int tops = 0, first = -1;
    for (uint32_t e = 0; e < r->m.n_ents; e++) {
        if (!strchr(r->m.ents[e].path, '/')) {
            tops++;
            if (first < 0)
                first = (int)e;
        }
    }
    snprintf(r->t->dest, sizeof r->t->dest, "%s", tops == 1 ? r->local[first] : r->root);
    return 0;
}

static void shutdown_streams(rsession_t *r)
{
    pthread_mutex_lock(&r->lock);
    r->closing = true;
    for (int i = 0; i < MAX_STREAMS; i++)
        if (r->data_fds[i] >= 0)
            shutdown(r->data_fds[i], SHUT_RDWR);
    /* Wait for the stream threads to let go of the session. */
    while (r->n_threads > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&r->cond, &r->lock, &ts);
    }
    pthread_mutex_unlock(&r->lock);
}

static void run_transfer(rsession_t *r)
{
    transfer_t *t = r->t;
    chan_t *ctrl = &r->conn->ctrl;
    char err[256] = "";

    if (!r->bench) {
        if (open_journal(r, err, sizeof err) < 0 || prepare(r) < 0) {
            if (!err[0])
                snprintf(err, sizeof err, "%s", r->io_err);
            send_simple(ctrl, MSG_ERROR, err);
            transfer_fail(t, TS_FAILED, "%s", err);
            return;
        }
    }

    /* Publish the session so data streams can find it, then accept. */
    pthread_mutex_lock(&g_sess_lock);
    r->refs++;   /* registry reference, dropped by session_unref at the end */
    r->next = g_sessions;
    g_sessions = r;
    pthread_mutex_unlock(&g_sess_lock);

    buf_t b = { 0 };
    buf_u8(&b, MSG_ACCEPT);
    buf_put(&b, r->id, SID_LEN);
    buf_u32(&b, r->m.n_chunks);
    size_t blen = r->bench ? 0 : (r->m.n_chunks + 7) / 8;
    buf_u32(&b, (uint32_t)blen);
    if (blen) {
        pthread_mutex_lock(&r->j.lock);
        buf_put(&b, r->j.bits, blen);
        pthread_mutex_unlock(&r->j.lock);
    }
    int rc = chan_send(ctrl, b.data, b.len);
    buf_free(&b);
    if (rc < 0) {
        transfer_fail(t, TS_FAILED, "connection lost");
        goto out;
    }
    transfer_set_state(t, TS_ACTIVE);
    LOGI("receiving %s from %s into %s", t->title, t->peer_name, t->dest);

    double last_flush = now_sec();
    for (;;) {
        if (atomic_load(&t->cancel)) {
            send_simple(ctrl, MSG_CANCEL, NULL);
            transfer_fail(t, TS_CANCELLED, "cancelled here — run the same send again to resume");
            break;
        }
        if (r->io_error) {
            send_simple(ctrl, MSG_ERROR, r->io_err);
            transfer_fail(t, TS_FAILED, "%s", r->io_err);
            break;
        }
        struct pollfd pfd = { .fd = r->conn->fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 500);
        if (now_sec() - last_flush > FLUSH_EVERY && r->have_journal) {
            journal_flush(&r->j, r->root_fd);
            last_flush = now_sec();
        }
        if (pr <= 0)
            continue;

        size_t n;
        uint8_t *msg = chan_recv(ctrl, &n);
        if (!msg) {
            transfer_fail(t, TS_FAILED, "connection lost — run the same send again to resume");
            break;
        }
        rd_t rd = { msg + 1, msg + n, false };
        if (msg[0] == MSG_CANCEL) {
            transfer_fail(t, TS_CANCELLED, "cancelled by sender");
            break;
        }
        if (msg[0] == MSG_ERROR) {
            char *e = rd_str(&rd);
            transfer_fail(t, TS_FAILED, "sender: %s", e ? e : "error");
            free(e);
            break;
        }
        if (msg[0] != MSG_DONE)
            continue;

        uint16_t ns = rd_u16(&rd);
        uint32_t counts[MAX_STREAMS] = { 0 };
        for (uint16_t i = 0; i < ns && i < MAX_STREAMS; i++)
            counts[i] = rd_u32(&rd);
        if (rd.err || wait_for_streams(r, counts, ns) < 0) {
            if (r->io_error)
                continue;   /* reported at the top of the loop */
            send_simple(ctrl, MSG_ERROR, "data streams stalled");
            transfer_fail(t, TS_FAILED, "data streams stalled — run the same send again to resume");
            break;
        }

        if (r->bench) {
            buf_t c = { 0 };
            buf_u8(&c, MSG_COMPLETE);
            buf_u64(&c, atomic_load(&t->bytes_done));
            chan_send(ctrl, c.data, c.len);
            buf_free(&c);
            transfer_set_state(t, TS_DONE);
            break;
        }

        buf_t need = { 0 };
        buf_u8(&need, MSG_NEED);
        buf_u32(&need, 0);
        uint32_t missing = 0;
        for (uint32_t i = 0; i < r->m.n_chunks && missing < MAX_NEED; i++) {
            if (!journal_test(&r->j, i)) {
                buf_u32(&need, i);
                missing++;
            }
        }
        if (missing) {
            put_u32(need.data + 1, missing);
            LOGI("transfer %d: asking for %u chunk(s) again", t->id, missing);
            rc = chan_send(ctrl, need.data, need.len);
            buf_free(&need);
            if (rc < 0) {
                transfer_fail(t, TS_FAILED, "connection lost");
                break;
            }
            continue;
        }
        buf_free(&need);

        transfer_set_state(t, TS_FINISHING);
        finalize_tree(r);
        journal_flush(&r->j, r->root_fd);
        if (r->root_fd >= 0)
            syncfs(r->root_fd);
        if (r->io_error)
            continue;
        buf_t c = { 0 };
        buf_u8(&c, MSG_COMPLETE);
        buf_u64(&c, atomic_load(&t->bytes_done));
        chan_send(ctrl, c.data, c.len);
        buf_free(&c);
        journal_close(&r->j, true);
        r->have_journal = false;
        transfer_set_state(t, TS_DONE);
        LOGI("received %s from %s", t->title, t->peer_name);
        break;
    }
out:
    if (r->have_journal)
        journal_flush(&r->j, r->root_fd);
    shutdown_streams(r);
    session_unref(r);   /* registry reference */
}
