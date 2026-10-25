/* The manifest lists every file, directory and symlink in a transfer.
 * Both sides derive the same chunk layout from it, so only the manifest
 * itself crosses the wire. */
#ifndef MANIFEST_H
#define MANIFEST_H

#include "beam.h"

enum { ENT_FILE = 0, ENT_DIR = 1, ENT_LINK = 2 };

typedef struct {
    uint8_t type;
    uint32_t mode;
    int64_t mtime_ns;
    uint64_t size;
    char *path;          /* relative, '/'-separated; first component is the top-level name */
    char *target;        /* symlink target */
    char *src;           /* sender only: path on the local disk */
    uint32_t n_chunks;   /* chunks that carry this file's bytes */
} entry_t;

enum { CHUNK_FILE = 0, CHUNK_BUNDLE = 1 };

typedef struct {
    uint8_t kind;
    uint32_t entry;      /* FILE: entry index; BUNDLE: start index into bundle_list */
    uint32_t count;      /* BUNDLE: number of files */
    uint64_t offset;     /* FILE: byte offset within the file */
    uint32_t len;        /* raw payload length */
} chunk_t;

typedef struct {
    entry_t *ents;
    uint32_t n_ents, cap_ents;
    chunk_t *chunks;
    uint32_t n_chunks;
    uint32_t *bundle_list;
    uint32_t n_bundle_list;
    uint64_t total_bytes;
    uint32_t chunk_size;
    uint32_t n_files;
} manifest_t;

/* Sender: add a file or directory (recursively). Returns 0 or -1 with errno. */
int manifest_add_path(manifest_t *m, const char *path);
void manifest_serialize(const manifest_t *m, buf_t *out);
/* Receiver: parse and validate. Rejects unsafe paths. */
int manifest_parse(manifest_t *m, const uint8_t *data, size_t len, char *err, size_t errn);
/* Both: compute the chunk layout. */
void manifest_layout(manifest_t *m, uint32_t chunk_size);
void manifest_free(manifest_t *m);
/* Short display name: "file.iso" or "photos + 2 more". */
void manifest_title(const manifest_t *m, char *out, size_t n);

#endif
