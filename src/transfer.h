/* Transfer registry: state, live statistics and history, shared by the
 * engine (sender.c / receiver.c) and the API. */
#ifndef TRANSFER_H
#define TRANSFER_H

#include "beam.h"
#include <stdatomic.h>
#include <cjson/cJSON.h>

typedef enum {
    TS_SCANNING,      /* sender: building the manifest */
    TS_CONNECTING,
    TS_WAITING,       /* waiting for the receiver (or the local user) to accept */
    TS_ACTIVE,
    TS_FINISHING,     /* all data sent, receiver verifying / finalising */
    TS_DONE,
    TS_FAILED,
    TS_CANCELLED,
    TS_REJECTED,
} tstate_t;

#define RATE_HIST 120

typedef struct transfer {
    int id;
    bool outgoing, bench, one_shot;
    char peer_name[128];
    char peer_fp[17];
    char title[256];
    char dest[1024];
    tstate_t state;
    char error[256];

    uint64_t total_bytes;
    uint32_t n_files, n_chunks;
    int streams, compress;
    bool resumed;
    int bench_secs;

    _Atomic uint64_t bytes_done;     /* payload bytes sent (sender) or written (receiver) */
    _Atomic uint64_t wire_bytes;     /* bytes actually on the wire, after compression */
    uint64_t skipped_bytes;          /* already present from an earlier attempt */

    int64_t started_ms, finished_ms;
    double t_start, t_last;
    uint64_t last_bytes;
    double rate;                     /* smoothed bytes/s */
    float hist[RATE_HIST];           /* bytes/s samples, oldest first */
    int n_hist;

    _Atomic bool cancel;
    int decision;                    /* incoming: 0 pending, 1 accept, -1 reject */
    pthread_cond_t cond;
    int refs;
    struct transfer *next;
} transfer_t;

extern pthread_mutex_t g_xfer_lock;

void transfers_init(void);
transfer_t *transfer_new(bool outgoing);
/* Looks up and takes a reference. */
transfer_t *transfer_get(int id);
void transfer_put(transfer_t *t);
void transfer_set_state(transfer_t *t, tstate_t s);
void transfer_fail(transfer_t *t, tstate_t s, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
bool transfer_finished(const transfer_t *t);
const char *tstate_name(tstate_t s);

/* Called twice a second by the daemon to update rates. */
void transfers_tick(void);
cJSON *transfer_json(transfer_t *t, bool with_hist);
cJSON *transfers_json(bool with_hist);
cJSON *history_json(void);

#endif
