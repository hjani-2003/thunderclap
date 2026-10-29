/* Peer-to-peer connection setup: hellos, handshakes, pairing and
 * one-shot codes. The transfer engine builds on the channels set up here. */
#ifndef PEER_H
#define PEER_H

#include "clap.h"
#include "crypt.h"
#include "config.h"

typedef struct {
    int fd;
    int cipher;
    uint8_t master[KEY_LEN];
    chan_t ctrl;
    /* who is on the other end */
    uint8_t peer_pk[KEY_LEN];
    char peer_name[128];
    char peer_fp[17];
    char peer_host[256];
    int peer_port;
    bool paired;     /* authenticated by a stored key (vs. a one-shot code) */
} session_conn_t;

/* Client side. Return 0 on success; on failure fill err. */
int connect_session(const char *host, int port, const uint8_t peer_pk[KEY_LEN], session_conn_t *s,
                    char *err, size_t errn);
int connect_with_code(const char *host, int port, const char *code, session_conn_t *s,
                      char *err, size_t errn);
int pair_join(const char *host, int port, const char *code, peer_t *out, char *err, size_t errn);
/* Opens data stream `idx` of a session; fills ch. */
int connect_data(const char *host, int port, const session_conn_t *s, const uint8_t session_id[SID_LEN],
                 uint16_t idx, chan_t *ch);

/* Responder side: arm a code that the next pairing / code transfer must use. */
void pair_code_arm(char out[15], int ttl_secs);
void recv_code_arm(char out[15], int ttl_secs);
/* Remaining seconds of the armed codes (0 when none). */
int pair_code_ttl(void);
int recv_code_ttl(void);

/* Handles one accepted TCP connection (runs in its own thread). */
void serve_connection(int fd, const struct sockaddr_storage *addr);

void session_close(session_conn_t *s);

#endif
