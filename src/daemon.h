#ifndef DAEMON_H
#define DAEMON_H

#include "beam.h"
#include <cjson/cJSON.h>

int daemon_main(void);

/* Client side of the unix-socket API (used by the CLI). Returns NULL when
 * the daemon is unreachable. */
cJSON *ipc_call(const char *cmd, cJSON *args);
bool ipc_ping(void);

/* Serves one HTTP connection (http.c). */
void http_serve(int fd);

#endif
