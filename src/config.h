/* Daemon configuration, identity and the paired-peer store. */
#ifndef CONFIG_H
#define CONFIG_H

#include "beam.h"
#include "crypt.h"

typedef struct {
    char home[1024];          /* ~/.config/beam or $BEAM_HOME */
    char sock_path[1100];     /* unix socket for the CLI */
    char name[128];           /* device name shown to peers */
    char download_dir[1024];
    int peer_port;
    int web_port;
    int streams;
    int compress;             /* COMPRESS_AUTO / COMPRESS_OFF / level */
    bool auto_accept;         /* accept transfers from paired peers without asking */
    identity_t id;
    char fp[17];
    char web_token[65];
} config_t;

extern config_t g_cfg;

/* Loads (creating defaults where missing). `home` may be NULL. */
int config_load(const char *home);
int config_save(void);
/* Path of a file inside the beam home directory. */
void config_path(const char *file, char *out, size_t n);

typedef struct {
    char name[128];
    uint8_t pk[KEY_LEN];
    char fp[17];
    char host[256];
    int port;
    int64_t paired_at;
} peer_t;

int peers_load(void);
/* Lookups copy the record into *out; return true when found. */
bool peers_find_pk(const uint8_t pk[KEY_LEN], peer_t *out);
bool peers_find(const char *name_or_fp, peer_t *out);
void peers_upsert(const peer_t *p);
void peers_set_host(const uint8_t pk[KEY_LEN], const char *host, int port);
bool peers_remove(const char *fp);
/* Snapshot of all peers; caller frees. */
peer_t *peers_list(size_t *n);

#endif
