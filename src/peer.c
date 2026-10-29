#include "peer.h"
#include "proto.h"
#include "receiver.h"

#include <sys/socket.h>
#include <sys/time.h>

#define HANDSHAKE_TIMEOUT 15

typedef struct {
    char code[15];
    double expires;
} code_slot_t;

static pthread_mutex_t slot_lock = PTHREAD_MUTEX_INITIALIZER;
static code_slot_t pair_slot, recv_slot;

static void arm(code_slot_t *slot, char out[15], int ttl)
{
    pthread_mutex_lock(&slot_lock);
    pair_code_generate(slot->code);
    slot->expires = now_sec() + ttl;
    memcpy(out, slot->code, 15);
    pthread_mutex_unlock(&slot_lock);
}

void pair_code_arm(char out[15], int ttl) { arm(&pair_slot, out, ttl); }
void recv_code_arm(char out[15], int ttl) { arm(&recv_slot, out, ttl); }

static int ttl_of(code_slot_t *slot)
{
    pthread_mutex_lock(&slot_lock);
    double left = slot->code[0] ? slot->expires - now_sec() : 0;
    pthread_mutex_unlock(&slot_lock);
    return left > 0 ? (int)left : 0;
}

int pair_code_ttl(void) { return ttl_of(&pair_slot); }
int recv_code_ttl(void) { return ttl_of(&recv_slot); }

/* Each code gets exactly one attempt: taking it disarms the slot, so a
 * guesser can't try again without the user generating a new code. */
static bool take_code(code_slot_t *slot, char out[15])
{
    pthread_mutex_lock(&slot_lock);
    bool ok = slot->code[0] && now_sec() < slot->expires;
    if (ok)
        memcpy(out, slot->code, 15);
    memset(slot, 0, sizeof *slot);
    pthread_mutex_unlock(&slot_lock);
    return ok;
}

static void set_timeout(int fd, int secs)
{
    struct timeval tv = { .tv_sec = secs };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

static void hello_header(buf_t *b, uint8_t kind)
{
    buf_put(b, PROTO_MAGIC, 4);
    buf_u8(b, PROTO_VERSION);
    buf_u8(b, kind);
}

static uint8_t my_caps(void) { return crypt_aes_available() ? CAP_AES256GCM : 0; }

void session_close(session_conn_t *s)
{
    chan_free(&s->ctrl);
    if (s->fd >= 0)
        close(s->fd);
    s->fd = -1;
    sodium_memzero(s->master, sizeof s->master);
}

/* ---- identity exchange (code and pairing flows) ---- */

static int send_ident(chan_t *c)
{
    buf_t b = { 0 };
    buf_u8(&b, MSG_IDENT);
    buf_put(&b, g_cfg.id.pk, KEY_LEN);
    buf_u16(&b, (uint16_t)g_cfg.peer_port);
    buf_str(&b, g_cfg.name);
    int rc = chan_send(c, b.data, b.len);
    buf_free(&b);
    return rc;
}

static int recv_ident(chan_t *c, session_conn_t *s)
{
    size_t n;
    uint8_t *m = chan_recv(c, &n);
    if (!m)
        return -1;
    rd_t r = { m, m + n, false };
    if (rd_u8(&r) != MSG_IDENT)
        return -1;
    const uint8_t *pk = rd_bytes(&r, KEY_LEN);
    uint16_t port = rd_u16(&r);
    char *name = rd_str(&r);
    if (r.err || !pk) {
        free(name);
        return -1;
    }
    memcpy(s->peer_pk, pk, KEY_LEN);
    key_fingerprint(pk, s->peer_fp);
    snprintf(s->peer_name, sizeof s->peer_name, "%s", name);
    s->peer_port = port;
    free(name);
    return 0;
}

/* ---- client side ---- */

static int finish_connect(session_conn_t *s, const char *host, int port, char *err, size_t errn)
{
    snprintf(s->peer_host, sizeof s->peer_host, "%s", host);
    s->peer_port = port;
    s->fd = tcp_connect(host, port, 5000);
    if (s->fd < 0) {
        snprintf(err, errn, "cannot connect to %s:%d: %s", host, port, strerror(errno));
        return -1;
    }
    set_timeout(s->fd, HANDSHAKE_TIMEOUT);
    return 0;
}

int connect_session(const char *host, int port, const uint8_t peer_pk[KEY_LEN], session_conn_t *s,
                    char *err, size_t errn)
{
    memset(s, 0, sizeof *s);
    if (finish_connect(s, host, port, err, errn) < 0)
        return -1;

    uint8_t esk[KEY_LEN], epk[KEY_LEN];
    crypto_box_keypair(epk, esk);
    buf_t b = { 0 };
    hello_header(&b, HELLO_SESSION);
    buf_put(&b, g_cfg.id.pk, KEY_LEN);
    buf_put(&b, epk, KEY_LEN);
    buf_u8(&b, my_caps());
    int rc = write_full(s->fd, b.data, b.len);
    buf_free(&b);

    uint8_t resp[1 + KEY_LEN + 1];
    if (rc < 0 || read_full(s->fd, resp, sizeof resp) < 0) {
        snprintf(err, errn, "handshake with %s failed (is thunderclap running there?)", host);
        goto fail;
    }
    if (resp[0] == ST_UNKNOWN) {
        snprintf(err, errn, "%s does not know this device — pair first with `clap pair`", host);
        goto fail;
    }
    if (resp[0] != ST_OK) {
        snprintf(err, errn, "peer refused the connection (status %d)", resp[0]);
        goto fail;
    }
    s->cipher = resp[1 + KEY_LEN] == CIPHER_AES ? CIPHER_AES : CIPHER_CHACHA;
    if (kk_initiator(&g_cfg.id, peer_pk, esk, epk, resp + 1, s->master) < 0) {
        snprintf(err, errn, "key exchange failed");
        goto fail;
    }
    sodium_memzero(esk, sizeof esk);
    memcpy(s->peer_pk, peer_pk, KEY_LEN);
    key_fingerprint(peer_pk, s->peer_fp);
    s->paired = true;
    chan_init(&s->ctrl, s->fd, s->cipher, s->master, "ctrl", 0, NULL, true);
    return 0;
fail:
    close(s->fd);
    s->fd = -1;
    return -1;
}

/* CPace handshake shared by code transfers and pairing. */
static int cpace_client(uint8_t kind, const char *host, int port, const char *code, session_conn_t *s,
                        char *err, size_t errn)
{
    memset(s, 0, sizeof *s);
    if (finish_connect(s, host, port, err, errn) < 0)
        return -1;
    uint8_t sid[SID_LEN];
    randombytes_buf(sid, sizeof sid);
    cpace_t cp;
    if (cpace_start(&cp, code, sid) < 0) {
        snprintf(err, errn, "invalid code");
        goto fail;
    }
    buf_t b = { 0 };
    hello_header(&b, kind);
    buf_put(&b, sid, SID_LEN);
    buf_put(&b, cp.Y, 32);
    buf_u8(&b, my_caps());
    int rc = write_full(s->fd, b.data, b.len);
    buf_free(&b);

    uint8_t resp[1 + 32 + 1];
    if (rc < 0 || read_full(s->fd, resp, sizeof resp) < 0) {
        snprintf(err, errn, "handshake with %s failed (is thunderclap running there?)", host);
        goto fail;
    }
    if (resp[0] != ST_OK) {
        snprintf(err, errn, "%s has no active code — run `clap %s` there first", host,
                 kind == HELLO_PAIR ? "pair" : "recv --code");
        goto fail;
    }
    s->cipher = resp[33] == CIPHER_AES ? CIPHER_AES : CIPHER_CHACHA;
    if (cpace_finish(&cp, resp + 1, true, s->master) < 0) {
        snprintf(err, errn, "key exchange failed");
        goto fail;
    }
    chan_init(&s->ctrl, s->fd, s->cipher, s->master, kind == HELLO_PAIR ? "pair" : "ctrl", 0, NULL, true);
    if (send_ident(&s->ctrl) < 0 || recv_ident(&s->ctrl, s) < 0) {
        snprintf(err, errn, "wrong code (the code is now used up — generate a new one)");
        chan_free(&s->ctrl);
        goto fail;
    }
    s->peer_port = port;
    return 0;
fail:
    close(s->fd);
    s->fd = -1;
    return -1;
}

int connect_with_code(const char *host, int port, const char *code, session_conn_t *s, char *err, size_t errn)
{
    return cpace_client(HELLO_CODE, host, port, code, s, err, errn);
}

int pair_join(const char *host, int port, const char *code, peer_t *out, char *err, size_t errn)
{
    session_conn_t s;
    if (cpace_client(HELLO_PAIR, host, port, code, &s, err, errn) < 0)
        return -1;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", s.peer_name);
    memcpy(out->pk, s.peer_pk, KEY_LEN);
    snprintf(out->host, sizeof out->host, "%s", host);
    out->port = port;
    out->paired_at = wall_ms();
    key_fingerprint(out->pk, out->fp);
    peers_upsert(out);
    session_close(&s);
    LOGI("paired with %s (%s)", out->name, out->fp);
    return 0;
}

int connect_data(const char *host, int port, const session_conn_t *s, const uint8_t session_id[SID_LEN],
                 uint16_t idx, chan_t *ch)
{
    int fd = tcp_connect(host, port, 5000);
    if (fd < 0)
        return -1;
    set_timeout(fd, HANDSHAKE_TIMEOUT);
    buf_t b = { 0 };
    hello_header(&b, HELLO_DATA);
    buf_put(&b, session_id, SID_LEN);
    buf_u16(&b, idx);
    int rc = write_full(fd, b.data, b.len);
    buf_free(&b);
    uint8_t resp[1 + SID_LEN];
    if (rc < 0 || read_full(fd, resp, sizeof resp) < 0 || resp[0] != ST_OK) {
        close(fd);
        return -1;
    }
    set_timeout(fd, 60);
    chan_init(ch, fd, s->cipher, s->master, "data", idx, resp + 1, true);
    return 0;
}

/* ---- server side ---- */

static void reply(int fd, uint8_t status, const void *body, size_t n)
{
    uint8_t buf[64] = { status };
    if (body)
        memcpy(buf + 1, body, n);
    write_full(fd, buf, 1 + (body ? n : 0));
}

static void serve_session(session_conn_t *s, const uint8_t *body)
{
    const uint8_t *their_pk = body, *their_eph = body + KEY_LEN;
    uint8_t caps = body[2 * KEY_LEN];
    peer_t p;
    if (!peers_find_pk(their_pk, &p)) {
        char fp[17];
        key_fingerprint(their_pk, fp);
        LOGW("rejecting connection from unpaired device %s (%s)", s->peer_host, fp);
        reply(s->fd, ST_UNKNOWN, NULL, 0);
        return;
    }
    s->cipher = (caps & CAP_AES256GCM) && crypt_aes_available() ? CIPHER_AES : CIPHER_CHACHA;
    uint8_t esk[KEY_LEN], resp[KEY_LEN + 1];
    crypto_box_keypair(resp, esk);
    resp[KEY_LEN] = (uint8_t)s->cipher;
    if (kk_responder(&g_cfg.id, their_pk, esk, resp, their_eph, s->master) < 0)
        return;
    sodium_memzero(esk, sizeof esk);
    reply(s->fd, ST_OK, resp, sizeof resp);

    memcpy(s->peer_pk, their_pk, KEY_LEN);
    snprintf(s->peer_name, sizeof s->peer_name, "%s", p.name);
    snprintf(s->peer_fp, sizeof s->peer_fp, "%s", p.fp);
    s->peer_port = p.port;
    s->paired = true;
    chan_init(&s->ctrl, s->fd, s->cipher, s->master, "ctrl", 0, NULL, false);
    receiver_run(s);
    /* Remember where the peer was last seen (its listening port is unchanged). */
    peers_set_host(their_pk, s->peer_host, p.port ? p.port : DEFAULT_PEER_PORT);
}

static void serve_cpace(session_conn_t *s, uint8_t kind, const uint8_t *body)
{
    char code[15];
    if (!take_code(kind == HELLO_PAIR ? &pair_slot : &recv_slot, code)) {
        reply(s->fd, ST_UNKNOWN, NULL, 0);
        return;
    }
    cpace_t cp;
    uint8_t caps = body[SID_LEN + 32];
    s->cipher = (caps & CAP_AES256GCM) && crypt_aes_available() ? CIPHER_AES : CIPHER_CHACHA;
    if (cpace_start(&cp, code, body) < 0 || cpace_finish(&cp, body + SID_LEN, false, s->master) < 0) {
        reply(s->fd, ST_UNKNOWN, NULL, 0);
        return;
    }
    uint8_t resp[33];
    memcpy(resp, cp.Y, 32);
    resp[32] = (uint8_t)s->cipher;
    reply(s->fd, ST_OK, resp, sizeof resp);

    chan_init(&s->ctrl, s->fd, s->cipher, s->master, kind == HELLO_PAIR ? "pair" : "ctrl", 0, NULL, false);
    if (recv_ident(&s->ctrl, s) < 0) {
        LOGW("%s from %s failed: wrong code", kind == HELLO_PAIR ? "pairing" : "code transfer", s->peer_host);
        return;
    }
    if (send_ident(&s->ctrl) < 0)
        return;

    if (kind == HELLO_PAIR) {
        peer_t p = { 0 };
        snprintf(p.name, sizeof p.name, "%s", s->peer_name);
        memcpy(p.pk, s->peer_pk, KEY_LEN);
        snprintf(p.host, sizeof p.host, "%s", s->peer_host);
        p.port = s->peer_port ? s->peer_port : DEFAULT_PEER_PORT;
        p.paired_at = wall_ms();
        peers_upsert(&p);
        LOGI("paired with %s (%s)", s->peer_name, s->peer_fp);
        return;
    }
    s->paired = peers_find_pk(s->peer_pk, NULL);
    receiver_run(s);
}

void serve_connection(int fd, const struct sockaddr_storage *addr)
{
    session_conn_t s;
    memset(&s, 0, sizeof s);
    s.fd = fd;
    sockaddr_str(addr, s.peer_host, sizeof s.peer_host);
    set_timeout(fd, HANDSHAKE_TIMEOUT);
    tcp_tune(fd);

    uint8_t hdr[6];
    if (read_full(fd, hdr, 6) < 0 || memcmp(hdr, PROTO_MAGIC, 4)) {
        close(fd);
        return;
    }
    if (hdr[4] != PROTO_VERSION) {
        reply(fd, ST_BAD_VERSION, NULL, 0);
        close(fd);
        return;
    }
    uint8_t body[2 * KEY_LEN + 1];
    switch (hdr[5]) {
    case HELLO_SESSION:
        if (read_full(fd, body, 2 * KEY_LEN + 1) == 0)
            serve_session(&s, body);
        break;
    case HELLO_CODE:
    case HELLO_PAIR:
        if (read_full(fd, body, SID_LEN + 32 + 1) == 0)
            serve_cpace(&s, hdr[5], body);
        break;
    case HELLO_DATA:
        if (read_full(fd, body, SID_LEN + 2) == 0) {
            set_timeout(fd, 60);
            receiver_data(fd, body, get_u16(body + SID_LEN));
            fd = -1;   /* receiver_data owns and closes it */
            s.fd = -1;
        }
        break;
    default:
        break;
    }
    session_close(&s);
}
