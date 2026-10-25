/* JSON command API, shared by the CLI (unix socket) and the web UI (HTTP). */
#ifndef API_H
#define API_H

#include <cjson/cJSON.h>

/* Runs a command; always returns an object (with "error" on failure). */
cJSON *api_call(const char *cmd, const cJSON *args);
/* Full snapshot pushed to the browser over SSE. */
cJSON *api_state(void);

#endif
