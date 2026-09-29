/* decode_lzma.c -- streaming LZMA1 / LZMA2 decoder (vendored 7-Zip SDK). */
#include <stdlib.h>
#include <string.h>

#include "decode.h"
#include "protocol.h"
#include "LzmaDec.h"
#include "Lzma2Dec.h"

#define LZMA_OUTBUF (256 * 1024)

static void *
sz_alloc(ISzAllocPtr p, size_t size) {
    (void)p;
    return malloc(size);
}

static void
sz_free(ISzAllocPtr p, void *address) {
    (void)p;
    free(address);
}

static const ISzAlloc g_alloc = { sz_alloc, sz_free };

typedef struct {
    int codec;
    CLzma2Dec lzma2;
    CLzmaDec lzma1;
    int allocated;
    int finished;
    uint64_t remaining; /* raw bytes left to produce (LZMA1); UINT64_MAX unknown */
    uint8_t *out;
} lzma_state_t;

static int
lzma_feed(void *v, const uint8_t *in, size_t in_len, int final,
          ps5sd_sink_fn sink, void *user) {
    lzma_state_t *s = v;
    const uint8_t *inp = in;
    size_t inleft = in_len;

    (void)final;

    while (inleft > 0 && !s->finished) {
        SizeT dest_len = LZMA_OUTBUF;
        SizeT src_len = inleft;
        ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
        SRes res;

        if (s->codec == PS5SD_CODEC_LZMA1 && s->remaining != UINT64_MAX) {
            if (s->remaining == 0) {
                s->finished = 1;
                break;
            }
            if (s->remaining < dest_len) {
                dest_len = (SizeT)s->remaining;
            }
        }

        if (s->codec == PS5SD_CODEC_LZMA2) {
            res = Lzma2Dec_DecodeToBuf(&s->lzma2, s->out, &dest_len, inp,
                                       &src_len, LZMA_FINISH_ANY, &status);
        } else {
            res = LzmaDec_DecodeToBuf(&s->lzma1, s->out, &dest_len, inp,
                                      &src_len, LZMA_FINISH_ANY, &status);
        }
        if (res != SZ_OK) {
            return -1;
        }

        inp += src_len;
        inleft -= src_len;

        if (s->codec == PS5SD_CODEC_LZMA1 && s->remaining != UINT64_MAX) {
            s->remaining -= dest_len;
        }
        if (dest_len && sink(user, s->out, dest_len)) {
            return -1;
        }
        if (status == LZMA_STATUS_FINISHED_WITH_MARK) {
            s->finished = 1;
            break;
        }
        if (src_len == 0 && dest_len == 0) {
            break; /* no progress: need more input */
        }
    }
    return 0;
}

static void
lzma_close(void *v) {
    lzma_state_t *s = v;

    if (s->allocated) {
        if (s->codec == PS5SD_CODEC_LZMA2) {
            Lzma2Dec_Free(&s->lzma2, &g_alloc);
        } else {
            LzmaDec_Free(&s->lzma1, &g_alloc);
        }
    }
    free(s->out);
    free(s);
}

ps5sd_decoder_t *
ps5sd_lzma_decoder_new(int codec, const uint8_t *props, size_t props_len,
                       uint64_t raw_size) {
    ps5sd_decoder_t *d = calloc(1, sizeof(*d));
    lzma_state_t *s = calloc(1, sizeof(*s));

    if (!d || !s) {
        free(d);
        free(s);
        return NULL;
    }
    s->codec = codec;
    s->out = malloc(LZMA_OUTBUF);
    if (!s->out) {
        lzma_close(s);
        free(d);
        return NULL;
    }

    if (codec == PS5SD_CODEC_LZMA2) {
        if (props_len < 1) {
            lzma_close(s);
            free(d);
            return NULL;
        }
        Lzma2Dec_Construct(&s->lzma2);
        s->allocated = 1;
        if (Lzma2Dec_Allocate(&s->lzma2, props[0], &g_alloc) != SZ_OK) {
            lzma_close(s);
            free(d);
            return NULL;
        }
        Lzma2Dec_Init(&s->lzma2);
        s->remaining = UINT64_MAX;
    } else {
        if (props_len < LZMA_PROPS_SIZE) {
            lzma_close(s);
            free(d);
            return NULL;
        }
        LzmaDec_Construct(&s->lzma1);
        s->allocated = 1;
        if (LzmaDec_Allocate(&s->lzma1, props, LZMA_PROPS_SIZE, &g_alloc)
            != SZ_OK) {
            lzma_close(s);
            free(d);
            return NULL;
        }
        LzmaDec_Init(&s->lzma1);
        s->remaining = raw_size ? raw_size : UINT64_MAX;
    }

    d->feed = lzma_feed;
    d->close = lzma_close;
    d->state = s;
    return d;
}
