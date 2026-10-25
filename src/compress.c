#include "compress.h"

#include <zstd.h>

#define SAMPLE_LEN (64 * 1024)
#define MIN_GAIN   0.90   /* send raw unless compression saves at least 10% */

struct compressor {
    ZSTD_CCtx *cctx;
    int level;            /* COMPRESS_AUTO, COMPRESS_OFF or a zstd level */
    uint8_t *sample;
    /* Auto mode: after several incompressible chunks in a row, only re-test
     * occasionally so videos and archives cost almost no CPU. */
    unsigned misses, skip;
};

compressor_t *compressor_new(int level)
{
    compressor_t *c = xcalloc(1, sizeof *c);
    c->level = level;
    if (level != COMPRESS_OFF) {
        c->cctx = ZSTD_createCCtx();
        c->sample = xmalloc(ZSTD_compressBound(SAMPLE_LEN));
    }
    return c;
}

void compressor_free(compressor_t *c)
{
    if (!c)
        return;
    ZSTD_freeCCtx(c->cctx);
    free(c->sample);
    free(c);
}

size_t compress_bound(size_t n) { return ZSTD_compressBound(n); }

size_t compressor_run(compressor_t *c, const void *src, size_t n, void *dst, size_t dstcap)
{
    if (c->level == COMPRESS_OFF || n < 512)
        return 0;
    int level = c->level;
    if (level == COMPRESS_AUTO) {
        if (c->skip) {
            c->skip--;
            return 0;
        }
        size_t sn = MIN(n, (size_t)SAMPLE_LEN);
        size_t r = ZSTD_compressCCtx(c->cctx, c->sample, ZSTD_compressBound(sn), src, sn, 1);
        if (ZSTD_isError(r) || r > sn * MIN_GAIN) {
            if (++c->misses >= 4)
                c->skip = 16;
            return 0;
        }
        c->misses = 0;
        level = 1;
    }
    size_t r = ZSTD_compressCCtx(c->cctx, dst, dstcap, src, n, level);
    if (ZSTD_isError(r) || r >= n * MIN_GAIN)
        return 0;
    return r;
}

void *decompressor_new(void) { return ZSTD_createDCtx(); }
void decompressor_free(void *d) { ZSTD_freeDCtx(d); }

int decompress_chunk(void *dctx, const void *src, size_t n, void *dst, size_t raw_len)
{
    size_t r = ZSTD_decompressDCtx(dctx, dst, raw_len, src, n);
    return ZSTD_isError(r) || r != raw_len ? -1 : 0;
}
