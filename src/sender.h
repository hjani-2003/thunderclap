#ifndef SENDER_H
#define SENDER_H

#include "transfer.h"
#include "crypt.h"

typedef struct {
    char host[256];
    int port;
    bool have_pk;              /* paired peer: authenticate with its stored key */
    uint8_t peer_pk[KEY_LEN];
    char peer_name[128];
    char code[32];             /* one-shot code transfer instead */
    char **paths;
    int n_paths;
    int streams;
    int compress;
    bool bench;
    int bench_secs;
} send_req_t;

/* Starts a background send; takes ownership of req->paths.
 * Returns the new transfer (caller must transfer_put it). */
transfer_t *sender_start(send_req_t *req);

#endif
