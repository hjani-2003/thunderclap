/* beam — common definitions shared by every module. */
#ifndef BEAM_H
#define BEAM_H

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <netinet/in.h>

#define BEAM_VERSION      "0.1.0"
#define DEFAULT_PEER_PORT 7878
#define DEFAULT_WEB_PORT  8080
#define DISCOVERY_PORT    7879

#define CHUNK_SIZE        (4u << 20)   /* 4 MiB */
#define SMALL_FILE_MAX    (1u << 20)   /* files below this are bundled */
#define MAX_STREAMS       32
#define MAX_RECORD        (CHUNK_SIZE + 64 * 1024)

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* ---- logging (util.c) ---- */
enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERR };
extern int g_log_level;
void logmsg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOGD(...) logmsg(LOG_DEBUG, __VA_ARGS__)
#define LOGI(...) logmsg(LOG_INFO, __VA_ARGS__)
#define LOGW(...) logmsg(LOG_WARN, __VA_ARGS__)
#define LOGE(...) logmsg(LOG_ERR, __VA_ARGS__)

/* ---- misc helpers (util.c) ---- */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t m);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
double now_sec(void);                         /* monotonic */
int64_t wall_ms(void);                        /* realtime, ms */
void hex_encode(const uint8_t *in, size_t n, char *out);   /* out: 2n+1 */
int hex_decode(const char *in, uint8_t *out, size_t n);    /* 0 on success */
int mkdir_p(const char *path, mode_t mode);
void human_bytes(double v, char *out, size_t n);
void human_rate(double bytes_per_sec, char *out, size_t n);

/* Blocking full-length I/O; return 0 on success, -1 on error/EOF. */
int read_full(int fd, void *buf, size_t n);
int write_full(int fd, const void *buf, size_t n);

/* ---- networking helpers (util.c) ---- */
int tcp_listen(const char *bind_addr, int port);
int tcp_connect(const char *host, int port, int timeout_ms);
void tcp_tune(int fd);                        /* big buffers etc. */
int parse_host_port(const char *s, char *host, size_t hn, int *port, int def_port);
void sockaddr_str(const struct sockaddr_storage *ss, char *out, size_t n);

/* ---- byte order helpers ---- */
static inline void put_u16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static inline void put_u32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static inline void put_u64(uint8_t *p, uint64_t v) { put_u32(p, v >> 32); put_u32(p + 4, (uint32_t)v); }
static inline uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t get_u32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint64_t get_u64(const uint8_t *p) { return (uint64_t)get_u32(p) << 32 | get_u32(p + 4); }

/* Growable byte buffer used for serialisation. */
typedef struct {
    uint8_t *data;
    size_t len, cap;
} buf_t;
void buf_reserve(buf_t *b, size_t extra);
void buf_put(buf_t *b, const void *p, size_t n);
void buf_u8(buf_t *b, uint8_t v);
void buf_u16(buf_t *b, uint16_t v);
void buf_u32(buf_t *b, uint32_t v);
void buf_u64(buf_t *b, uint64_t v);
void buf_str(buf_t *b, const char *s);       /* u16 length + bytes */
void buf_free(buf_t *b);

/* Bounds-checked reader over a byte range. `err` latches on overrun. */
typedef struct {
    const uint8_t *p, *end;
    bool err;
} rd_t;
uint8_t rd_u8(rd_t *r);
uint16_t rd_u16(rd_t *r);
uint32_t rd_u32(rd_t *r);
uint64_t rd_u64(rd_t *r);
const uint8_t *rd_bytes(rd_t *r, size_t n);
char *rd_str(rd_t *r);                        /* malloc'd, NUL-terminated */

#endif
