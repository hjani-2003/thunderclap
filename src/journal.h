/* Resume journal: a bitmap of chunks that are safely on disk.
 * File layout: "BEAMJ001" | n_chunks u32 | meta_len u32 | meta | bitmap */
#ifndef JOURNAL_H
#define JOURNAL_H

#include "beam.h"

typedef struct {
    char *path;
    int fd;
    uint32_t n;
    uint32_t n_done;
    uint8_t *bits;
    size_t bits_off;
    bool dirty;
    pthread_mutex_t lock;
} journal_t;

/* Opens or creates the journal at `path`. On create, `meta` is stored;
 * on open, `meta` is replaced with the stored bytes and *resumed is set. */
int journal_open(journal_t *j, const char *path, uint32_t n_chunks, buf_t *meta, bool *resumed);
bool journal_test(journal_t *j, uint32_t idx);
/* Returns true if the bit was newly set. */
bool journal_set(journal_t *j, uint32_t idx);
void journal_clear(journal_t *j, uint32_t idx);
uint32_t journal_done(journal_t *j);
/* Makes data durable (syncfs on `sync_fd`), then writes the bitmap. */
int journal_flush(journal_t *j, int sync_fd);
void journal_close(journal_t *j, bool remove_file);

#endif
