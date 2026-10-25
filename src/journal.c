#include "journal.h"

#include <fcntl.h>
#include <sys/stat.h>

#define JMAGIC "BEAMJ001"

int journal_open(journal_t *j, const char *path, uint32_t n_chunks, buf_t *meta, bool *resumed)
{
    memset(j, 0, sizeof *j);
    pthread_mutex_init(&j->lock, NULL);
    j->path = xstrdup(path);
    j->n = n_chunks;
    j->bits = xcalloc((n_chunks + 7) / 8 + 1, 1);
    *resumed = false;

    j->fd = open(path, O_RDWR | O_CLOEXEC);
    if (j->fd >= 0) {
        uint8_t hdr[16];
        struct stat st;
        if (read_full(j->fd, hdr, 16) == 0 && !memcmp(hdr, JMAGIC, 8) && get_u32(hdr + 8) == n_chunks &&
            fstat(j->fd, &st) == 0) {
            uint32_t mlen = get_u32(hdr + 12);
            size_t blen = (n_chunks + 7) / 8;
            if ((uint64_t)st.st_size == 16ull + mlen + blen) {
                buf_t stored = { 0 };
                buf_reserve(&stored, mlen);
                stored.len = mlen;
                if (read_full(j->fd, stored.data, mlen) == 0 && read_full(j->fd, j->bits, blen) == 0) {
                    buf_free(meta);
                    *meta = stored;
                    j->bits_off = 16 + mlen;
                    for (uint32_t i = 0; i < n_chunks; i++)
                        if (j->bits[i / 8] & (1u << (i % 8)))
                            j->n_done++;
                    *resumed = true;
                    return 0;
                }
                buf_free(&stored);
                memset(j->bits, 0, (n_chunks + 7) / 8);
            }
        }
        LOGW("journal %s is unusable, starting over", path);
        close(j->fd);
    }

    j->fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (j->fd < 0)
        return -1;
    uint8_t hdr[16];
    memcpy(hdr, JMAGIC, 8);
    put_u32(hdr + 8, n_chunks);
    put_u32(hdr + 12, (uint32_t)meta->len);
    j->bits_off = 16 + meta->len;
    if (write_full(j->fd, hdr, 16) || write_full(j->fd, meta->data, meta->len) ||
        write_full(j->fd, j->bits, (n_chunks + 7) / 8))
        return -1;
    return 0;
}

bool journal_test(journal_t *j, uint32_t idx)
{
    pthread_mutex_lock(&j->lock);
    bool r = j->bits[idx / 8] & (1u << (idx % 8));
    pthread_mutex_unlock(&j->lock);
    return r;
}

bool journal_set(journal_t *j, uint32_t idx)
{
    pthread_mutex_lock(&j->lock);
    bool fresh = !(j->bits[idx / 8] & (1u << (idx % 8)));
    if (fresh) {
        j->bits[idx / 8] |= (uint8_t)(1u << (idx % 8));
        j->n_done++;
        j->dirty = true;
    }
    pthread_mutex_unlock(&j->lock);
    return fresh;
}

void journal_clear(journal_t *j, uint32_t idx)
{
    pthread_mutex_lock(&j->lock);
    if (j->bits[idx / 8] & (1u << (idx % 8))) {
        j->bits[idx / 8] &= (uint8_t)~(1u << (idx % 8));
        j->n_done--;
        j->dirty = true;
    }
    pthread_mutex_unlock(&j->lock);
}

uint32_t journal_done(journal_t *j)
{
    pthread_mutex_lock(&j->lock);
    uint32_t r = j->n_done;
    pthread_mutex_unlock(&j->lock);
    return r;
}

int journal_flush(journal_t *j, int sync_fd)
{
    pthread_mutex_lock(&j->lock);
    if (!j->dirty) {
        pthread_mutex_unlock(&j->lock);
        return 0;
    }
    size_t blen = (j->n + 7) / 8;
    uint8_t *snap = xmalloc(blen + 1);
    memcpy(snap, j->bits, blen);
    j->dirty = false;
    pthread_mutex_unlock(&j->lock);

    /* Data first, then the bitmap that claims it — so a crash never leaves
     * the journal pointing at bytes that did not reach the disk. */
    if (sync_fd >= 0)
        syncfs(sync_fd);
    int rc = pwrite(j->fd, snap, blen, (off_t)j->bits_off) == (ssize_t)blen ? 0 : -1;
    free(snap);
    return rc;
}

void journal_close(journal_t *j, bool remove_file)
{
    if (j->fd >= 0)
        close(j->fd);
    if (remove_file && j->path)
        unlink(j->path);
    free(j->path);
    free(j->bits);
    pthread_mutex_destroy(&j->lock);
    memset(j, 0, sizeof *j);
    j->fd = -1;
}
