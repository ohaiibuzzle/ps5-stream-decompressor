/* decode_zstd.c -- streaming zstd decoder. */
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

#include "decode.h"

#define ZSTD_OUTBUF (256 * 1024)

typedef struct {
    ZSTD_DCtx *dctx;
    uint8_t *out;
} zstd_state_t;

static int
zstd_feed(void *v, const uint8_t *in, size_t in_len, int final,
          ps5sd_sink_fn sink, void *user) {
    zstd_state_t *s = v;
    ZSTD_inBuffer ib = { in, in_len, 0 };

    (void)final;

    while (ib.pos < ib.size) {
        ZSTD_outBuffer ob = { s->out, ZSTD_OUTBUF, 0 };
        size_t r = ZSTD_decompressStream(s->dctx, &ob, &ib);
        size_t before = ib.pos;

        if (ZSTD_isError(r)) {
            return -1;
        }
        if (ob.pos && sink(user, s->out, ob.pos)) {
            return -1;
        }
        /* r == 0 means a frame completed; loop handles concatenated frames. */
        if (before == ib.pos && ob.pos == 0) {
            break; /* no progress: need more input */
        }
    }
    return 0;
}

static void
zstd_close(void *v) {
    zstd_state_t *s = v;

    if (s->dctx) {
        ZSTD_freeDCtx(s->dctx);
    }
    free(s->out);
    free(s);
}

ps5sd_decoder_t *
ps5sd_zstd_decoder_new(void) {
    ps5sd_decoder_t *d = calloc(1, sizeof(*d));
    zstd_state_t *s = calloc(1, sizeof(*s));

    if (!d || !s) {
        free(d);
        free(s);
        return NULL;
    }
    s->dctx = ZSTD_createDCtx();
    s->out = malloc(ZSTD_OUTBUF);
    if (!s->dctx || !s->out) {
        zstd_close(s);
        free(d);
        return NULL;
    }
    /* Allow large windows (long-distance matching produces up to 128 MiB). */
    ZSTD_DCtx_setParameter(s->dctx, ZSTD_d_windowLogMax, 31);

    d->feed = zstd_feed;
    d->close = zstd_close;
    d->state = s;
    return d;
}
