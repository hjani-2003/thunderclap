/* thunderclap wire protocol.
 *
 * Every TCP connection starts with a plaintext hello:
 *     "CLAP" | version u8 | kind u8 | kind-specific fields
 * and the responder answers with a status byte plus its own fields.
 * After that, everything travels in AEAD records (see crypt.h):
 *     len u32 (big-endian, covers ciphertext + 16-byte tag) | ciphertext | tag
 * Each record's plaintext starts with a one-byte message type.
 */
#ifndef PROTO_H
#define PROTO_H

#define PROTO_MAGIC   "CLAP"
#define PROTO_VERSION 1

/* Hello kinds */
enum {
    HELLO_SESSION = 1, /* paired peers: static_pk[32] eph_pk[32] caps u8          */
    HELLO_CODE    = 2, /* one-shot code transfer: sid[16] Y[32] caps u8           */
    HELLO_PAIR    = 3, /* pairing: sid[16] Y[32] caps u8                          */
    HELLO_DATA    = 4, /* data stream: session_id[16] stream u16                  */
};

/* Hello response status */
enum {
    ST_OK          = 0,
    ST_UNKNOWN     = 1, /* unknown peer / no pending code / unknown session */
    ST_BAD_VERSION = 2,
    ST_BUSY        = 3,
};

/* Capability bits sent in hellos; the responder answers with the chosen cipher. */
#define CAP_AES256GCM 0x01
enum { CIPHER_CHACHA = 0, CIPHER_AES = 1 };

/* Control-channel messages */
enum {
    MSG_IDENT    = 1,  /* pk[32] name str                                        */
    MSG_OFFER    = 2,  /* flags u8 compress i8 streams u8 chunk u32 total u64
                          bench_secs u32 name str manifest_len u32 manifest     */
    MSG_ACCEPT   = 3,  /* session_id[16] n_chunks u32 bitmap_len u32 bitmap     */
    MSG_REJECT   = 4,  /* reason str                                             */
    MSG_DONE     = 5,  /* n u16, then n × u32 chunks sent per stream             */
    MSG_NEED     = 6,  /* n u32, then n × u32 chunk indices                      */
    MSG_COMPLETE = 7,  /* bytes u64 (bench: bytes received)                      */
    MSG_ERROR    = 8,  /* reason str                                             */
    MSG_CANCEL   = 9,
    MSG_PING     = 10,
};

/* Data-stream messages */
enum {
    MSG_CHUNK = 20,    /* idx u32 flags u8 raw_len u32 xxh64 u64 payload        */
    MSG_BENCH = 21,    /* payload (discarded)                                    */
};
#define CHUNK_HDR_LEN (1 + 4 + 1 + 4 + 8)
#define CHUNK_F_ZSTD  0x01

/* Offer flags */
#define OFFER_F_BENCH 0x01

#endif
