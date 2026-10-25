/* Identity keys, pairing (CPace PAKE), paired-peer handshake and AEAD channels. */
#ifndef CRYPT_H
#define CRYPT_H

#include "beam.h"
#include <sodium.h>

#define KEY_LEN 32
#define SID_LEN 16
#define TAG_LEN 16

typedef struct {
    uint8_t pk[KEY_LEN];
    uint8_t sk[KEY_LEN];
} identity_t;

int crypt_init(void);
bool crypt_aes_available(void);

/* Loads `path`, creating a new X25519 identity there if it does not exist. */
int identity_load(const char *path, identity_t *id);
/* 16 hex chars identifying a public key, e.g. for display and discovery. */
void key_fingerprint(const uint8_t pk[KEY_LEN], char out[17]);

/* ---- CPace over ristretto255 ---- */
typedef struct {
    uint8_t scalar[crypto_core_ristretto255_SCALARBYTES];
    uint8_t Y[crypto_core_ristretto255_BYTES];
    uint8_t sid[SID_LEN];
} cpace_t;

/* Random code "1234-5678-9012"; `out` needs 15 bytes. */
void pair_code_generate(char out[15]);
int cpace_start(cpace_t *c, const char *code, const uint8_t sid[SID_LEN]);
/* `initiator_Y` / `responder_Y` order the transcript; one of them is c->Y. */
int cpace_finish(cpace_t *c, const uint8_t peer_Y[32], bool initiator, uint8_t isk[KEY_LEN]);

/* ---- paired-peer handshake (Noise-KK style) ---- */
int kk_initiator(const identity_t *me, const uint8_t peer_pk[KEY_LEN],
                 const uint8_t eph_sk[KEY_LEN], const uint8_t eph_pk[KEY_LEN],
                 const uint8_t peer_eph[KEY_LEN], uint8_t master[KEY_LEN]);
int kk_responder(const identity_t *me, const uint8_t peer_pk[KEY_LEN],
                 const uint8_t eph_sk[KEY_LEN], const uint8_t eph_pk[KEY_LEN],
                 const uint8_t peer_eph[KEY_LEN], uint8_t master[KEY_LEN]);

/* ---- AEAD channel ---- */
typedef struct {
    uint8_t key[KEY_LEN];
    crypto_aead_aes256gcm_state aes;
    uint64_t ctr;
} cdir_t;

typedef struct {
    int fd;
    int cipher;            /* CIPHER_* */
    cdir_t tx, rx;
    uint8_t *rbuf;
    size_t rcap;
} chan_t;

/* Derive the two directional keys for a channel.
 * label/idx/salt make the key unique: ctrl uses ("ctrl", 0, NULL),
 * data stream i uses ("data", i, salt). */
void chan_init(chan_t *c, int fd, int cipher, const uint8_t master[KEY_LEN],
               const char *label, uint16_t idx, const uint8_t *salt, bool initiator);
void chan_free(chan_t *c);

/* Encrypts buf[0..len) in place and sends it as one record. */
int chan_send(chan_t *c, uint8_t *buf, size_t len);
/* Convenience: copies into a temporary buffer first. */
int chan_send_copy(chan_t *c, const void *buf, size_t len);
/* Receives one record; returns a pointer into the channel's buffer (valid
 * until the next call) or NULL on error/EOF/authentication failure. */
uint8_t *chan_recv(chan_t *c, size_t *len);

#endif
