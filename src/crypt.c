#include "crypt.h"
#include "proto.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>

static bool g_aes;

int crypt_init(void)
{
    if (sodium_init() < 0)
        return -1;
    g_aes = crypto_aead_aes256gcm_is_available();
    return 0;
}

bool crypt_aes_available(void) { return g_aes; }

int identity_load(const char *path, identity_t *id)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        uint8_t raw[2 * KEY_LEN];
        int rc = read_full(fd, raw, sizeof raw);
        close(fd);
        if (rc < 0)
            return -1;
        memcpy(id->pk, raw, KEY_LEN);
        memcpy(id->sk, raw + KEY_LEN, KEY_LEN);
        sodium_memzero(raw, sizeof raw);
        return 0;
    }
    crypto_box_keypair(id->pk, id->sk);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    int rc = write_full(fd, id->pk, KEY_LEN) | write_full(fd, id->sk, KEY_LEN);
    close(fd);
    return rc;
}

void key_fingerprint(const uint8_t pk[KEY_LEN], char out[17])
{
    uint8_t h[8];
    crypto_generichash(h, sizeof h, pk, KEY_LEN, (const uint8_t *)"beam-fp", 7);
    hex_encode(h, sizeof h, out);
}

/* ---- CPace ---- */

void pair_code_generate(char out[15])
{
    int k = 0;
    for (int i = 0; i < 12; i++) {
        if (i && i % 4 == 0)
            out[k++] = '-';
        out[k++] = (char)('0' + randombytes_uniform(10));
    }
    out[k] = 0;
}

int cpace_start(cpace_t *c, const char *code, const uint8_t sid[SID_LEN])
{
    /* Only the digits matter, so "1234 5678-9012" and "123456789012" match. */
    char digits[64];
    size_t n = 0;
    for (const char *p = code; *p && n < sizeof digits - 1; p++)
        if (*p >= '0' && *p <= '9')
            digits[n++] = *p;
    if (n < 6)
        return -1;

    memcpy(c->sid, sid, SID_LEN);
    uint8_t h[crypto_core_ristretto255_HASHBYTES], G[crypto_core_ristretto255_BYTES];
    crypto_generichash_state st;
    crypto_generichash_init(&st, NULL, 0, sizeof h);
    crypto_generichash_update(&st, (const uint8_t *)"beam-cpace-v1", 13);
    crypto_generichash_update(&st, sid, SID_LEN);
    crypto_generichash_update(&st, (const uint8_t *)digits, n);
    crypto_generichash_final(&st, h, sizeof h);
    crypto_core_ristretto255_from_hash(G, h);

    crypto_core_ristretto255_scalar_random(c->scalar);
    if (crypto_scalarmult_ristretto255(c->Y, c->scalar, G) != 0)
        return -1;
    return 0;
}

int cpace_finish(cpace_t *c, const uint8_t peer_Y[32], bool initiator, uint8_t isk[KEY_LEN])
{
    uint8_t K[crypto_core_ristretto255_BYTES];
    if (!crypto_core_ristretto255_is_valid_point(peer_Y))
        return -1;
    if (crypto_scalarmult_ristretto255(K, c->scalar, peer_Y) != 0)
        return -1;
    crypto_generichash_state st;
    crypto_generichash_init(&st, NULL, 0, KEY_LEN);
    crypto_generichash_update(&st, (const uint8_t *)"beam-cpace-isk", 14);
    crypto_generichash_update(&st, K, sizeof K);
    crypto_generichash_update(&st, c->sid, SID_LEN);
    crypto_generichash_update(&st, initiator ? c->Y : peer_Y, 32);
    crypto_generichash_update(&st, initiator ? peer_Y : c->Y, 32);
    crypto_generichash_final(&st, isk, KEY_LEN);
    sodium_memzero(K, sizeof K);
    sodium_memzero(c->scalar, sizeof c->scalar);
    return 0;
}

/* ---- KK handshake ---- */

static int kk_mix(const uint8_t dh[3][KEY_LEN], const uint8_t *is, const uint8_t *rs,
                  const uint8_t *ie, const uint8_t *re, uint8_t master[KEY_LEN])
{
    crypto_generichash_state st;
    crypto_generichash_init(&st, NULL, 0, KEY_LEN);
    crypto_generichash_update(&st, (const uint8_t *)"beam-kk-v1", 10);
    crypto_generichash_update(&st, dh[0], 3 * KEY_LEN);
    crypto_generichash_update(&st, is, KEY_LEN);
    crypto_generichash_update(&st, rs, KEY_LEN);
    crypto_generichash_update(&st, ie, KEY_LEN);
    crypto_generichash_update(&st, re, KEY_LEN);
    crypto_generichash_final(&st, master, KEY_LEN);
    return 0;
}

int kk_initiator(const identity_t *me, const uint8_t peer_pk[KEY_LEN],
                 const uint8_t eph_sk[KEY_LEN], const uint8_t eph_pk[KEY_LEN],
                 const uint8_t peer_eph[KEY_LEN], uint8_t master[KEY_LEN])
{
    uint8_t dh[3][KEY_LEN];
    if (crypto_scalarmult(dh[0], eph_sk, peer_eph) || crypto_scalarmult(dh[1], me->sk, peer_eph) ||
        crypto_scalarmult(dh[2], eph_sk, peer_pk))
        return -1;
    kk_mix((const uint8_t(*)[KEY_LEN])dh, me->pk, peer_pk, eph_pk, peer_eph, master);
    sodium_memzero(dh, sizeof dh);
    return 0;
}

int kk_responder(const identity_t *me, const uint8_t peer_pk[KEY_LEN],
                 const uint8_t eph_sk[KEY_LEN], const uint8_t eph_pk[KEY_LEN],
                 const uint8_t peer_eph[KEY_LEN], uint8_t master[KEY_LEN])
{
    uint8_t dh[3][KEY_LEN];
    if (crypto_scalarmult(dh[0], eph_sk, peer_eph) || crypto_scalarmult(dh[1], eph_sk, peer_pk) ||
        crypto_scalarmult(dh[2], me->sk, peer_eph))
        return -1;
    kk_mix((const uint8_t(*)[KEY_LEN])dh, peer_pk, me->pk, peer_eph, eph_pk, master);
    sodium_memzero(dh, sizeof dh);
    return 0;
}

/* ---- channels ---- */

static void derive(const uint8_t master[KEY_LEN], const char *label, uint16_t idx,
                   const uint8_t *salt, uint8_t dir, uint8_t out[KEY_LEN])
{
    uint8_t tail[3] = { (uint8_t)(idx >> 8), (uint8_t)idx, dir };
    crypto_generichash_state st;
    crypto_generichash_init(&st, master, KEY_LEN, KEY_LEN);
    crypto_generichash_update(&st, (const uint8_t *)label, strlen(label));
    crypto_generichash_update(&st, tail, sizeof tail);
    if (salt)
        crypto_generichash_update(&st, salt, SID_LEN);
    crypto_generichash_final(&st, out, KEY_LEN);
}

static void cdir_setup(cdir_t *d, int cipher)
{
    d->ctr = 0;
    if (cipher == CIPHER_AES)
        crypto_aead_aes256gcm_beforenm(&d->aes, d->key);
}

void chan_init(chan_t *c, int fd, int cipher, const uint8_t master[KEY_LEN],
               const char *label, uint16_t idx, const uint8_t *salt, bool initiator)
{
    memset(c, 0, sizeof *c);
    c->fd = fd;
    c->cipher = cipher;
    derive(master, label, idx, salt, initiator ? 0 : 1, c->tx.key);
    derive(master, label, idx, salt, initiator ? 1 : 0, c->rx.key);
    cdir_setup(&c->tx, cipher);
    cdir_setup(&c->rx, cipher);
}

void chan_free(chan_t *c)
{
    sodium_memzero(&c->tx, sizeof c->tx);
    sodium_memzero(&c->rx, sizeof c->rx);
    free(c->rbuf);
    c->rbuf = NULL;
    c->rcap = 0;
}

static void make_nonce(uint64_t ctr, uint8_t nonce[12])
{
    memset(nonce, 0, 4);
    put_u64(nonce + 4, ctr);
}

int chan_send(chan_t *c, uint8_t *buf, size_t len)
{
    uint8_t hdr[4], tag[TAG_LEN], nonce[12];
    if (len + TAG_LEN > MAX_RECORD)
        return -1;
    put_u32(hdr, (uint32_t)(len + TAG_LEN));
    make_nonce(c->tx.ctr++, nonce);
    if (c->cipher == CIPHER_AES)
        crypto_aead_aes256gcm_encrypt_detached_afternm(buf, tag, NULL, buf, len, hdr, 4, NULL, nonce, &c->tx.aes);
    else
        crypto_aead_chacha20poly1305_ietf_encrypt_detached(buf, tag, NULL, buf, len, hdr, 4, NULL, nonce, c->tx.key);

    struct iovec iov[3] = { { hdr, 4 }, { buf, len }, { tag, TAG_LEN } };
    size_t total = 4 + len + TAG_LEN;
    int idx = 0;
    while (total) {
        ssize_t w = writev(c->fd, iov + idx, 3 - idx);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        total -= (size_t)w;
        while (idx < 3 && (size_t)w >= iov[idx].iov_len) {
            w -= (ssize_t)iov[idx].iov_len;
            idx++;
        }
        if (idx < 3) {
            iov[idx].iov_base = (uint8_t *)iov[idx].iov_base + w;
            iov[idx].iov_len -= (size_t)w;
        }
    }
    return 0;
}

int chan_send_copy(chan_t *c, const void *buf, size_t len)
{
    uint8_t stackbuf[1024];
    uint8_t *tmp = len <= sizeof stackbuf ? stackbuf : xmalloc(len);
    memcpy(tmp, buf, len);
    int rc = chan_send(c, tmp, len);
    if (tmp != stackbuf)
        free(tmp);
    return rc;
}

uint8_t *chan_recv(chan_t *c, size_t *len)
{
    uint8_t hdr[4], nonce[12];
    if (read_full(c->fd, hdr, 4) < 0)
        return NULL;
    uint32_t n = get_u32(hdr);
    if (n < TAG_LEN || n > MAX_RECORD)
        return NULL;
    if (c->rcap < n) {
        c->rbuf = xrealloc(c->rbuf, n);
        c->rcap = n;
    }
    if (read_full(c->fd, c->rbuf, n) < 0)
        return NULL;
    size_t mlen = n - TAG_LEN;
    make_nonce(c->rx.ctr++, nonce);
    int rc;
    if (c->cipher == CIPHER_AES)
        rc = crypto_aead_aes256gcm_decrypt_detached_afternm(c->rbuf, NULL, c->rbuf, mlen, c->rbuf + mlen,
                                                            hdr, 4, nonce, &c->rx.aes);
    else
        rc = crypto_aead_chacha20poly1305_ietf_decrypt_detached(c->rbuf, NULL, c->rbuf, mlen, c->rbuf + mlen,
                                                                hdr, 4, nonce, c->rx.key);
    if (rc != 0) {
        LOGW("record authentication failed");
        return NULL;
    }
    *len = mlen;
    return c->rbuf;
}
