/* Adaptive per-chunk zstd compression. */
#ifndef COMPRESS_H
#define COMPRESS_H

#include "clap.h"

#define COMPRESS_AUTO -1
#define COMPRESS_OFF  0

typedef struct compressor compressor_t;

compressor_t *compressor_new(int level);
void compressor_free(compressor_t *c);
/* Tries to compress src into dst (capacity dstcap). Returns the compressed
 * size, or 0 if the chunk should be sent raw (incompressible / disabled). */
size_t compressor_run(compressor_t *c, const void *src, size_t n, void *dst, size_t dstcap);
size_t compress_bound(size_t n);

/* Returns 0 only if the data decompresses to exactly raw_len bytes. */
int decompress_chunk(void *dctx, const void *src, size_t n, void *dst, size_t raw_len);
void *decompressor_new(void);
void decompressor_free(void *dctx);

#endif
