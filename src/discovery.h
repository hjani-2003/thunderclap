/* LAN discovery: every daemon announces itself with small UDP beacons
 * (broadcast + multicast) and listens for others. */
#ifndef DISCOVERY_H
#define DISCOVERY_H

#include "beam.h"
#include <cjson/cJSON.h>

int discovery_open(void);
void discovery_beacon(int fd);
void discovery_handle(int fd);
void discovery_expire(void);
cJSON *discovery_json(void);

/* Best (fastest-interface) address for a discovered peer, by name or
 * fingerprint. Fills fp (17 bytes) when non-NULL. */
bool discovery_lookup(const char *name_or_fp, char *host, size_t hn, int *port, char *fp, char *name, size_t nn);

/* Local interfaces with their link speed, for display. */
cJSON *netif_json(void);

#endif
