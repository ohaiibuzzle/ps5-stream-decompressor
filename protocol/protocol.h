/*
 * protocol.h -- wire format shared by the ps5-stream-decompressor client and
 * server.
 *
 * The protocol is a small binary framing over a single TCP connection:
 *
 *   client                                                        server
 *   ------                                                        ------
 *   HELLO            ------------------------------------------->
 *   DATA (repeated)  ------------------------------------------->
 *   ...              <------------------------------------------- STATUS
 *
 * All integers are unsigned and little-endian.
 */
#ifndef PS5SD_PROTOCOL_H
#define PS5SD_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define PS5SD_MAGIC      0x53355350u /* "PS5S" in little-endian */
#define PS5SD_VERSION    1u
#define PS5SD_DEFAULT_PORT 9080
#define PS5SD_DEFAULT_DIR  "/data/uploads"

#define PS5SD_NAME_MAX    1024
#define PS5SD_PROPS_MAX   64

/* Transfer codec. Passthrough codecs (LZMA1/2) carry the archive's own
 * compressed bytes; ZSTD is used when the client recompresses on the fly. */
enum ps5sd_codec {
    PS5SD_CODEC_RAW   = 0,
    PS5SD_CODEC_ZSTD  = 1,
    PS5SD_CODEC_LZMA2 = 2,
    PS5SD_CODEC_LZMA1 = 3,
};

/* HELLO flags. */
#define PS5SD_FLAG_CHUNKED   0x01u /* payload split into independent frames */
#define PS5SD_FLAG_HAS_PROPS 0x02u /* coder properties are present */
#define PS5SD_FLAG_QUERY     0x04u /* report the size of an existing partial file */

/* DATA frame flags. */
enum ps5sd_frame_flags {
    PS5SD_FRAME_DATA  = 0,
    PS5SD_FRAME_FINAL = 1,
    PS5SD_FRAME_RAW   = 2, /* payload is stored verbatim */
    PS5SD_FRAME_ABORT = 3, /* client aborted; server should discard output */
};

/* STATUS message types. */
enum ps5sd_status_type {
    PS5SD_STATUS_PROGRESS = 0,
    PS5SD_STATUS_DONE     = 1,
    PS5SD_STATUS_ERROR    = 2,
};

/* Error codes carried in STATUS.code. */
enum ps5sd_error {
    PS5SD_OK              = 0,
    PS5SD_ERR_BAD_MAGIC   = 1,
    PS5SD_ERR_BAD_VERSION = 2,
    PS5SD_ERR_BAD_NAME    = 3,
    PS5SD_ERR_BAD_CODEC   = 4,
    PS5SD_ERR_IO          = 5,
    PS5SD_ERR_DECODE      = 6,
    PS5SD_ERR_PROTOCOL    = 7,
    PS5SD_ERR_EXISTS      = 8,
    PS5SD_ERR_BAD_TOKEN   = 9,
};

/* Fixed part of the HELLO message (everything before name+props). */
#define PS5SD_HELLO_FIXED 44

static inline void ps5sd_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static inline void ps5sd_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void ps5sd_put_u64(uint8_t *p, uint64_t v) {
    ps5sd_put_u32(p, (uint32_t)v);
    ps5sd_put_u32(p + 4, (uint32_t)(v >> 32));
}

static inline uint16_t ps5sd_get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t ps5sd_get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t ps5sd_get_u64(const uint8_t *p) {
    return (uint64_t)ps5sd_get_u32(p) |
           ((uint64_t)ps5sd_get_u32(p + 4) << 32);
}

/* Parsed view of a HELLO message. name and props point into the caller's
 * receive buffer and are NOT null-terminated. */
typedef struct {
    uint8_t  version;
    uint8_t  codec;
    uint8_t  flags;
    uint16_t name_len;
    uint16_t props_len;
    uint32_t chunk_raw_size;
    uint32_t token;
    uint64_t resume_offset;
    uint64_t raw_size;
    uint64_t payload_size;
    const uint8_t *name;
    const uint8_t *props;
} ps5sd_hello_t;

static inline void ps5sd_hello_serialize(uint8_t out[PS5SD_HELLO_FIXED],
                                         uint8_t version, uint8_t codec,
                                         uint8_t flags,
                                         uint16_t name_len,
                                         uint16_t props_len,
                                         uint32_t chunk_raw_size,
                                         uint32_t token,
                                         uint64_t resume_offset,
                                         uint64_t raw_size,
                                         uint64_t payload_size) {
    ps5sd_put_u32(out + 0, PS5SD_MAGIC);
    out[4] = version;
    out[5] = codec;
    out[6] = flags;
    out[7] = 0;
    ps5sd_put_u16(out + 8, name_len);
    ps5sd_put_u16(out + 10, props_len);
    ps5sd_put_u32(out + 12, chunk_raw_size);
    ps5sd_put_u32(out + 16, token);
    ps5sd_put_u64(out + 20, resume_offset);
    ps5sd_put_u64(out + 28, raw_size);
    ps5sd_put_u64(out + 36, payload_size);
}

/* Parse the fixed part. Returns 0 on success, negative on malformed input. */
static inline int ps5sd_hello_parse_fixed(const uint8_t in[PS5SD_HELLO_FIXED],
                                          ps5sd_hello_t *h) {
    if (ps5sd_get_u32(in + 0) != PS5SD_MAGIC) {
        return -1;
    }
    memset(h, 0, sizeof(*h));
    h->version        = in[4];
    h->codec          = in[5];
    h->flags          = in[6];
    h->name_len       = ps5sd_get_u16(in + 8);
    h->props_len      = ps5sd_get_u16(in + 10);
    h->chunk_raw_size = ps5sd_get_u32(in + 12);
    h->token          = ps5sd_get_u32(in + 16);
    h->resume_offset  = ps5sd_get_u64(in + 20);
    h->raw_size       = ps5sd_get_u64(in + 28);
    h->payload_size   = ps5sd_get_u64(in + 36);
    if (h->name_len > PS5SD_NAME_MAX || h->props_len > PS5SD_PROPS_MAX) {
        return -1;
    }
    return 0;
}

#define PS5SD_FRAME_HEADER 9

static inline void ps5sd_frame_serialize(uint8_t out[PS5SD_FRAME_HEADER],
                                         uint32_t comp_len, uint32_t raw_len,
                                         uint8_t flags) {
    ps5sd_put_u32(out + 0, comp_len);
    ps5sd_put_u32(out + 4, raw_len);
    out[8] = flags;
}

static inline void ps5sd_frame_parse(const uint8_t in[PS5SD_FRAME_HEADER],
                                     uint32_t *comp_len, uint32_t *raw_len,
                                     uint8_t *flags) {
    *comp_len = ps5sd_get_u32(in + 0);
    *raw_len  = ps5sd_get_u32(in + 4);
    *flags    = in[8];
}

#define PS5SD_STATUS_LEN 21

static inline void ps5sd_status_serialize(uint8_t out[PS5SD_STATUS_LEN],
                                          uint8_t type, uint64_t received,
                                          uint64_t written, uint32_t code) {
    out[0] = type;
    ps5sd_put_u64(out + 1, received);
    ps5sd_put_u64(out + 9, written);
    ps5sd_put_u32(out + 17, code);
}

static inline void ps5sd_status_parse(const uint8_t in[PS5SD_STATUS_LEN],
                                      uint8_t *type, uint64_t *received,
                                      uint64_t *written, uint32_t *code) {
    *type     = in[0];
    *received = ps5sd_get_u64(in + 1);
    *written  = ps5sd_get_u64(in + 9);
    *code     = ps5sd_get_u32(in + 17);
}

#endif /* PS5SD_PROTOCOL_H */
