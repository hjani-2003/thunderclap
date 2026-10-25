#include "sender.h"
#include "peer.h"
#include "proto.h"
#include "manifest.h"
#include "compress.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <xxhash.h>

/* Work queue of chunk indices shared by the stream workers. */
typedef struct {
    uint32_t *items;
    size_t n, head, cap;
    size_t inflight;
    bool closed;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} chunkq_t;

typedef struct sender sender_t;

typedef struct {
    sender_t *s;
    int idx;
    pthread_t th;
    chan_t ch;
    bool connected;
    uint32_t sent;            /* chunks sent on this stream (cumulative) */
} worker_t;

struct sender {
    send_req_t req;
    transfer_t *t;
    manifest_t m;
    session_conn_t conn;
    uint8_t session_id[SID_LEN];
    chunkq_t q;
    worker_t workers[MAX_STREAMS];
    int n_workers;
    double bench_until;
    pthread_mutex_t err_lock;
    char err[256];
};

/* ---- queue ---- */

static void q_push(chunkq_t *q, const uint32_t *items, size_t n)
{
    pthread_mutex_lock(&q->mu);
    if (q->n + n > q->cap) {
        q->cap = MAX(q->cap * 2, q->n + n);
        q->items = xrealloc(q->items, q->cap * sizeof *q->items);
    }
    memcpy(q->items + q->n, items, n * sizeof *items);
    q->n += n;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

static bool q_pop(chunkq_t *q, uint32_t *out)
{
    pthread_mutex_lock(&q->mu);
    while (q->head == q->n && !q->closed)
        pthread_cond_wait(&q->cv, &q->mu);
    bool ok = q->head < q->n && !q->closed;
    if (ok) {
        *out = q->items[q->head++];
        q->inflight++;
    }
    pthread_mutex_unlock(&q->mu);
    return ok;
}

static void q_done(chunkq_t *q)
{
    pthread_mutex_lock(&q->mu);
    q->inflight--;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

static void q_close(chunkq_t *q)
{
    pthread_mutex_lock(&q->mu);
    q->closed = true;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

/* Waits up to `ms` for the round to drain. */
static bool q_wait_idle(chunkq_t *q, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    pthread_mutex_lock(&q->mu);
    while (!(q->head == q->n && q->inflight == 0) && !q->closed)
        if (pthread_cond_timedwait(&q->cv, &q->mu, &ts) != 0)
            break;
    bool idle = q->head == q->n && q->inflight == 0;
    pthread_mutex_unlock(&q->mu);
    return idle;
}

static void set_err(sender_t *s, const char *fmt, const char *arg)
{
    pthread_mutex_lock(&s->err_lock);
    if (!s->err[0])
        snprintf(s->err, sizeof s->err, fmt, arg, strerror(errno));
    pthread_mutex_unlock(&s->err_lock);
    q_close(&s->q);
}

/* ---- workers ---- */

static int pread_full(int fd, uint8_t *p, size_t n, off_t off)
{
    while (n) {
        ssize_t r = pread(fd, p, n, off);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            return -1;
        if (r == 0) {
            errno = ESTALE;   /* file shrank while we were sending it */
            return -1;
        }
        p += r;
        n -= (size_t)r;
        off += r;
    }
    return 0;
}

static int read_chunk(sender_t *s, uint32_t idx, uint8_t *dst, int *cache_fd, uint32_t *cache_ent)
{
    const chunk_t *c = &s->m.chunks[idx];
    if (c->kind == CHUNK_FILE) {
        const entry_t *e = &s->m.ents[c->entry];
        if (*cache_fd < 0 || *cache_ent != c->entry) {
            if (*cache_fd >= 0)
                close(*cache_fd);
            *cache_fd = open(e->src, O_RDONLY | O_CLOEXEC);
            *cache_ent = c->entry;
            if (*cache_fd < 0) {
                set_err(s, "cannot open %s: %s", e->src);
                return -1;
            }
            posix_fadvise(*cache_fd, 0, 0, POSIX_FADV_SEQUENTIAL);
        }
        if (pread_full(*cache_fd, dst, c->len, (off_t)c->offset) < 0) {
            set_err(s, "cannot read %s: %s", e->src);
            return -1;
        }
        return 0;
    }
    for (uint32_t k = 0; k < c->count; k++) {
        const entry_t *e = &s->m.ents[s->m.bundle_list[c->entry + k]];
        int fd = open(e->src, O_RDONLY | O_CLOEXEC);
        if (fd < 0 || pread_full(fd, dst, e->size, 0) < 0) {
            set_err(s, "cannot read %s: %s", e->src);
            if (fd >= 0)
                close(fd);
            return -1;
        }
        close(fd);
        dst += e->size;
    }
    return 0;
}

static void *worker_main(void *arg)
{
    worker_t *w = arg;
    sender_t *s = w->s;
    transfer_t *t = s->t;

    if (s->req.bench) {
        size_t len = 1 + s->m.chunk_size;
        uint8_t *buf = xmalloc(len);
        while (!atomic_load(&t->cancel) && now_sec() < s->bench_until && !s->q.closed) {
            buf[0] = MSG_BENCH;
            randombytes_buf(buf + 1, 64);   /* contents don't matter; avoid trivially-compressible zeros */
            if (chan_send(&w->ch, buf, len) < 0) {
                set_err(s, "data stream lost%s: %s", "");
                break;
            }
            w->sent++;
            atomic_fetch_add(&t->bytes_done, len - 1);
            atomic_fetch_add(&t->wire_bytes, len - 1);
        }
        free(buf);
        return NULL;
    }

    compressor_t *comp = compressor_new(s->req.compress);
    uint8_t *raw = xmalloc(CHUNK_HDR_LEN + s->m.chunk_size);
    size_t zcap = compress_bound(s->m.chunk_size);
    uint8_t *z = s->req.compress != COMPRESS_OFF ? xmalloc(CHUNK_HDR_LEN + zcap) : NULL;
    int cache_fd = -1;
    uint32_t cache_ent = 0, idx;

    while (q_pop(&s->q, &idx)) {
        if (atomic_load(&t->cancel)) {
            q_done(&s->q);
            break;
        }
        const chunk_t *c = &s->m.chunks[idx];
        uint8_t *data = raw + CHUNK_HDR_LEN;
        if (read_chunk(s, idx, data, &cache_fd, &cache_ent) < 0) {
            q_done(&s->q);
            break;
        }
        uint64_t hash = XXH3_64bits(data, c->len);
        size_t zn = z ? compressor_run(comp, data, c->len, z + CHUNK_HDR_LEN, zcap) : 0;
        uint8_t *msg = zn ? z : raw;
        size_t plen = zn ? zn : c->len;
        msg[0] = MSG_CHUNK;
        put_u32(msg + 1, idx);
        msg[5] = zn ? CHUNK_F_ZSTD : 0;
        put_u32(msg + 6, c->len);
        put_u64(msg + 10, hash);
        if (chan_send(&w->ch, msg, CHUNK_HDR_LEN + plen) < 0) {
            set_err(s, "data stream lost%s: %s", "");
            q_done(&s->q);
            break;
        }
        w->sent++;
        atomic_fetch_add(&t->bytes_done, c->len);
        atomic_fetch_add(&t->wire_bytes, plen);
        q_done(&s->q);
    }
    if (cache_fd >= 0)
        close(cache_fd);
    compressor_free(comp);
    free(raw);
    free(z);
    return NULL;
}

/* ---- control ---- */

static int send_offer(sender_t *s)
{
    buf_t mb = { 0 };
    manifest_serialize(&s->m, &mb);
    buf_t b = { 0 };
    buf_u8(&b, MSG_OFFER);
    buf_u8(&b, s->req.bench ? OFFER_F_BENCH : 0);
    buf_u8(&b, (uint8_t)(int8_t)s->req.compress);
    buf_u8(&b, (uint8_t)s->req.streams);
    buf_u32(&b, s->m.chunk_size);
    buf_u64(&b, s->m.total_bytes);
    buf_u32(&b, (uint32_t)s->req.bench_secs);
    buf_str(&b, s->t->title);
    buf_u32(&b, (uint32_t)mb.len);
    buf_put(&b, mb.data, mb.len);
    buf_free(&mb);
    if (b.len > MAX_RECORD - 64) {
        buf_free(&b);
        errno = EFBIG;
        return -2;
    }
    int rc = chan_send(&s->conn.ctrl, b.data, b.len);
    buf_free(&b);
    return rc;
}

/* Waits for a control message while watching for local cancellation.
 * Returns the message, or NULL (with *cancelled set when that was why). */
static uint8_t *wait_msg(sender_t *s, size_t *n, bool *cancelled)
{
    *cancelled = false;
    for (;;) {
        if (atomic_load(&s->t->cancel)) {
            *cancelled = true;
            return NULL;
        }
        struct pollfd pfd = { .fd = s->conn.fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 300);
        if (pr < 0 && errno != EINTR)
            return NULL;
        if (pr > 0)
            return chan_recv(&s->conn.ctrl, n);
    }
}

static void stop_workers(sender_t *s)
{
    q_close(&s->q);
    for (int i = 0; i < s->n_workers; i++)
        if (s->workers[i].connected)
            shutdown(s->workers[i].ch.fd, SHUT_RDWR);
    for (int i = 0; i < s->n_workers; i++) {
        worker_t *w = &s->workers[i];
        if (w->connected) {
            pthread_join(w->th, NULL);
            close(w->ch.fd);
            chan_free(&w->ch);
        }
    }
    s->n_workers = 0;
}

static void send_ctrl_simple(sender_t *s, uint8_t type)
{
    chan_send_copy(&s->conn.ctrl, &type, 1);
}

static void run(sender_t *s)
{
    transfer_t *t = s->t;
    send_req_t *rq = &s->req;
    char err[256];

    /* 1. scan */
    if (!rq->bench) {
        for (int i = 0; i < rq->n_paths; i++) {
            if (manifest_add_path(&s->m, rq->paths[i]) < 0) {
                transfer_fail(t, TS_FAILED, "cannot read %s: %s", rq->paths[i], strerror(errno));
                return;
            }
        }
        manifest_title(&s->m, t->title, sizeof t->title);
    } else {
        snprintf(t->title, sizeof t->title, "link benchmark");
    }
    manifest_layout(&s->m, CHUNK_SIZE);
    t->total_bytes = s->m.total_bytes;
    t->n_files = s->m.n_files;
    t->n_chunks = s->m.n_chunks;

    /* 2. connect + authenticate */
    transfer_set_state(t, TS_CONNECTING);
    int rc = rq->code[0] ? connect_with_code(rq->host, rq->port, rq->code, &s->conn, err, sizeof err)
                         : connect_session(rq->host, rq->port, rq->peer_pk, &s->conn, err, sizeof err);
    if (rc < 0) {
        transfer_fail(t, TS_FAILED, "%s", err);
        return;
    }
    if (s->conn.peer_name[0])
        snprintf(t->peer_name, sizeof t->peer_name, "%s", s->conn.peer_name);
    snprintf(t->peer_fp, sizeof t->peer_fp, "%s", s->conn.peer_fp);
    struct timeval tv = { 0 };
    setsockopt(s->conn.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    /* 3. offer */
    rc = send_offer(s);
    if (rc < 0) {
        transfer_fail(t, TS_FAILED, rc == -2 ? "too many files for one transfer — split it up" : "connection lost");
        return;
    }
    transfer_set_state(t, TS_WAITING);
    size_t n;
    bool cancelled;
    uint8_t *msg = wait_msg(s, &n, &cancelled);
    if (!msg) {
        if (cancelled)
            send_ctrl_simple(s, MSG_CANCEL);
        transfer_fail(t, cancelled ? TS_CANCELLED : TS_FAILED, cancelled ? "cancelled" : "connection lost");
        return;
    }
    rd_t rd = { msg + 1, msg + n, false };
    if (msg[0] == MSG_REJECT || msg[0] == MSG_ERROR) {
        char *why = rd_str(&rd);
        transfer_fail(t, msg[0] == MSG_REJECT ? TS_REJECTED : TS_FAILED, "%s", why ? why : "refused");
        free(why);
        return;
    }
    if (msg[0] != MSG_ACCEPT) {
        transfer_fail(t, TS_FAILED, "unexpected reply from receiver");
        return;
    }
    const uint8_t *sid = rd_bytes(&rd, SID_LEN);
    uint32_t nchunks = rd_u32(&rd);
    uint32_t blen = rd_u32(&rd);
    const uint8_t *bits = rd_bytes(&rd, blen);
    if (rd.err || nchunks != s->m.n_chunks || (blen && blen != (nchunks + 7) / 8)) {
        transfer_fail(t, TS_FAILED, "receiver disagrees about the chunk layout");
        return;
    }
    memcpy(s->session_id, sid, SID_LEN);

    /* 4. queue whatever the receiver doesn't already have */
    uint32_t *todo = xmalloc((nchunks + 1) * sizeof *todo);
    uint32_t ntodo = 0;
    uint64_t have = 0;
    for (uint32_t i = 0; i < nchunks; i++) {
        if (blen && (bits[i / 8] & (1u << (i % 8))))
            have += s->m.chunks[i].len;
        else
            todo[ntodo++] = i;
    }
    q_push(&s->q, todo, ntodo);
    free(todo);
    t->skipped_bytes = have;
    atomic_store(&t->bytes_done, have);
    t->resumed = have > 0;
    if (have)
        LOGI("resuming: receiver already has %.1f%%", 100.0 * have / MAX(1, s->m.total_bytes));

    /* 5. data streams */
    int want = rq->bench ? rq->streams : (int)MIN((uint32_t)rq->streams, MAX(1u, ntodo));
    s->bench_until = now_sec() + rq->bench_secs;
    for (int i = 0; i < want; i++) {
        worker_t *w = &s->workers[i];
        w->s = s;
        w->idx = i;
        if (connect_data(rq->host, rq->port, &s->conn, s->session_id, (uint16_t)i, &w->ch) < 0) {
            if (i == 0) {
                stop_workers(s);
                transfer_fail(t, TS_FAILED, "could not open data streams");
                return;
            }
            break;   /* run with fewer streams */
        }
        w->connected = true;
        s->n_workers = i + 1;
    }
    t->streams = s->n_workers;
    transfer_set_state(t, TS_ACTIVE);
    for (int i = 0; i < s->n_workers; i++)
        pthread_create(&s->workers[i].th, NULL, worker_main, &s->workers[i]);

    /* 6. rounds: send everything, report DONE, resend whatever is missing */
    for (;;) {
        bool idle = false;
        while (!idle) {
            if (atomic_load(&t->cancel) || s->err[0])
                break;
            if (rq->bench) {
                /* Bench workers exit on their own when time is up. */
                idle = now_sec() >= s->bench_until + 0.5;
                usleep(100000);
            } else {
                idle = q_wait_idle(&s->q, 300);
            }
            struct pollfd pfd = { .fd = s->conn.fd, .events = POLLIN };
            if (!idle && poll(&pfd, 1, 0) == 1)
                break;   /* receiver says something (cancel / error) mid-round */
        }
        if (atomic_load(&t->cancel)) {
            send_ctrl_simple(s, MSG_CANCEL);
            transfer_fail(t, TS_CANCELLED, "cancelled — send again to resume");
            break;
        }
        if (s->err[0]) {
            /* A broken data stream usually means the receiver hit a problem
             * (disk full, permissions) and said why on the control channel. */
            struct pollfd pfd = { .fd = s->conn.fd, .events = POLLIN };
            if (poll(&pfd, 1, 1000) == 1 && (msg = chan_recv(&s->conn.ctrl, &n)) && n >= 1 &&
                (msg[0] == MSG_ERROR || msg[0] == MSG_CANCEL)) {
                rd = (rd_t){ msg + 1, msg + n, false };
                char *why = msg[0] == MSG_ERROR ? rd_str(&rd) : NULL;
                transfer_fail(t, msg[0] == MSG_ERROR ? TS_FAILED : TS_CANCELLED, "receiver: %s",
                              why ? why : "cancelled");
                free(why);
                break;
            }
            buf_t e = { 0 };
            buf_u8(&e, MSG_ERROR);
            buf_str(&e, s->err);
            chan_send(&s->conn.ctrl, e.data, e.len);
            buf_free(&e);
            transfer_fail(t, TS_FAILED, "%s", s->err);
            break;
        }
        if (idle) {
            if (rq->bench) {
                /* Join so every stream's count is final. */
                for (int i = 0; i < s->n_workers; i++)
                    pthread_join(s->workers[i].th, NULL);
            }
            buf_t d = { 0 };
            buf_u8(&d, MSG_DONE);
            buf_u16(&d, (uint16_t)s->n_workers);
            for (int i = 0; i < s->n_workers; i++)
                buf_u32(&d, s->workers[i].sent);
            rc = chan_send(&s->conn.ctrl, d.data, d.len);
            buf_free(&d);
            if (rc < 0) {
                transfer_fail(t, TS_FAILED, "connection lost — send again to resume");
                break;
            }
            transfer_set_state(t, TS_FINISHING);
            if (rq->bench) {
                for (int i = 0; i < s->n_workers; i++) {
                    close(s->workers[i].ch.fd);
                    chan_free(&s->workers[i].ch);
                    s->workers[i].connected = false;
                }
            }
        }
        msg = wait_msg(s, &n, &cancelled);
        if (!msg) {
            if (cancelled) {
                send_ctrl_simple(s, MSG_CANCEL);
                transfer_fail(t, TS_CANCELLED, "cancelled — send again to resume");
            } else {
                transfer_fail(t, TS_FAILED, "connection lost — send again to resume");
            }
            break;
        }
        rd = (rd_t){ msg + 1, msg + n, false };
        if (msg[0] == MSG_COMPLETE) {
            transfer_set_state(t, TS_DONE);
            char rate[32];
            human_rate(t->rate, rate, sizeof rate);
            LOGI("sent %s to %s (%s)", t->title, t->peer_name, rate);
            break;
        }
        if (msg[0] == MSG_NEED) {
            uint32_t cnt = rd_u32(&rd);
            const uint8_t *list = rd_bytes(&rd, (size_t)cnt * 4);
            if (rd.err) {
                transfer_fail(t, TS_FAILED, "bad resend request");
                break;
            }
            uint32_t *items = xmalloc(((size_t)cnt + 1) * sizeof *items);
            uint32_t k = 0;
            for (uint32_t i = 0; i < cnt; i++) {
                uint32_t idx = get_u32(list + 4 * i);
                if (idx < s->m.n_chunks)
                    items[k++] = idx;
            }
            q_push(&s->q, items, k);
            free(items);
            transfer_set_state(t, TS_ACTIVE);
            continue;
        }
        if (msg[0] == MSG_CANCEL) {
            transfer_fail(t, TS_CANCELLED, "cancelled by receiver");
            break;
        }
        if (msg[0] == MSG_ERROR) {
            char *why = rd_str(&rd);
            transfer_fail(t, TS_FAILED, "receiver: %s", why ? why : "error");
            free(why);
            break;
        }
    }
    if (!rq->bench || s->workers[0].connected)
        stop_workers(s);
}

static void *sender_thread(void *arg)
{
    sender_t *s = arg;
    run(s);
    session_close(&s->conn);
    manifest_free(&s->m);
    for (int i = 0; i < s->req.n_paths; i++)
        free(s->req.paths[i]);
    free(s->req.paths);
    free(s->q.items);
    pthread_mutex_destroy(&s->q.mu);
    pthread_cond_destroy(&s->q.cv);
    pthread_mutex_destroy(&s->err_lock);
    transfer_put(s->t);
    free(s);
    return NULL;
}

transfer_t *sender_start(send_req_t *req)
{
    sender_t *s = xcalloc(1, sizeof *s);
    s->req = *req;
    s->req.streams = MAX(1, MIN(s->req.streams, MAX_STREAMS));
    s->conn.fd = -1;
    pthread_mutex_init(&s->q.mu, NULL);
    pthread_cond_init(&s->q.cv, NULL);
    pthread_mutex_init(&s->err_lock, NULL);

    transfer_t *t = transfer_new(true);
    t->bench = req->bench;
    t->bench_secs = req->bench_secs;
    t->one_shot = req->code[0] != 0;
    t->streams = s->req.streams;
    t->compress = req->compress;
    snprintf(t->peer_name, sizeof t->peer_name, "%s", req->peer_name[0] ? req->peer_name : req->host);
    s->t = t;

    pthread_mutex_lock(&g_xfer_lock);
    t->refs++;   /* one for the caller */
    pthread_mutex_unlock(&g_xfer_lock);

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &attr, sender_thread, s);
    pthread_attr_destroy(&attr);
    return t;
}
