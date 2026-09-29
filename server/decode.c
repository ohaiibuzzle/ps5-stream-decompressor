/* decode.c -- decoder dispatch. */
#include <stddef.h>
#include <stdlib.h>

#include "decode.h"
#include "protocol.h"

/* Provided by decode_zstd.c / decode_lzma.c. */
ps5sd_decoder_t *ps5sd_zstd_decoder_new(void);
ps5sd_decoder_t *ps5sd_lzma_decoder_new(int codec, const uint8_t *props,
                                        size_t props_len, uint64_t raw_size);

ps5sd_decoder_t *
ps5sd_decoder_open(int codec, const uint8_t *props, size_t props_len,
                   uint64_t raw_size) {
    switch (codec) {
    case PS5SD_CODEC_ZSTD:
        return ps5sd_zstd_decoder_new();
    case PS5SD_CODEC_LZMA2:
    case PS5SD_CODEC_LZMA1:
        return ps5sd_lzma_decoder_new(codec, props, props_len, raw_size);
    default:
        return NULL;
    }
}

void
ps5sd_decoder_free(ps5sd_decoder_t *d) {
    if (d) {
        if (d->close && d->state) {
            d->close(d->state);
        }
        free(d);
    }
}
