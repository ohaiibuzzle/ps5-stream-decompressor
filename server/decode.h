/* decode.h -- streaming decompressor abstraction shared by the server. */
#ifndef PS5SD_DECODE_H
#define PS5SD_DECODE_H

#include <stddef.h>
#include <stdint.h>

/* Sink is called with decompressed bytes. Return non-zero to abort. */
typedef int (*ps5sd_sink_fn)(void *user, const void *buf, size_t len);

typedef struct ps5sd_decoder ps5sd_decoder_t;

struct ps5sd_decoder {
    /* Consume a chunk of compressed input; emit decompressed bytes to sink.
     * `final` marks the last input chunk. Returns 0 on success, -1 on error. */
    int (*feed)(void *state, const uint8_t *in, size_t in_len, int final,
                ps5sd_sink_fn sink, void *user);
    void (*close)(void *state);
    void *state;
};

/* Open a decoder for the given transfer codec.
 *   codec     PS5SD_CODEC_ZSTD / LZMA2 / LZMA1
 *   props     coder properties (LZMA1: 5 bytes, LZMA2: 1 byte)
 *   raw_size  expected uncompressed size (used to terminate LZMA1 streams)
 * Returns NULL on unsupported codec / bad props / OOM. */
ps5sd_decoder_t *ps5sd_decoder_open(int codec, const uint8_t *props,
                                    size_t props_len, uint64_t raw_size);

/* Release a decoder returned by ps5sd_decoder_open(). */
void ps5sd_decoder_free(ps5sd_decoder_t *d);

#endif /* PS5SD_DECODE_H */
