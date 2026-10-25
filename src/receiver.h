#ifndef RECEIVER_H
#define RECEIVER_H

#include "peer.h"

/* Runs the receiving side of a session after the handshake: waits for an
 * OFFER, accepts it (automatically or after asking the user), then drives
 * the transfer until it completes, fails or is cancelled. */
void receiver_run(session_conn_t *s);

/* Serves one data stream; takes ownership of fd. */
void receiver_data(int fd, const uint8_t session_id[SID_LEN], uint16_t stream);

#endif
