/* compress.c -- streaming zstd encoder used by the client. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

#include "compress.h"

struct compress_ctx {
    ZSTD_CCtx *cctx;
    size_t chunk_raw;
    size_t fill;
    uint8_t *inbuf;
    uint8_t *outbuf;
    size_t outcap;
    compress_out_fn out;
    void *user;
};

compress_ctx_t *
compress_new(int level, int threads, int window_log, size_t chunk_raw,
             compress_out_fn out, void *user) {
    compress_ctx_t *c;

    if (chunk_raw == 0) {
        return NULL;
    }
    c = calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->cctx = ZSTD_createCCtx();
    c->inbuf = malloc(chunk_raw);
    c->outcap = ZSTD_compressBound(chunk_raw);
    c->outbuf = malloc(c->outcap);
    c->chunk_raw = chunk_raw;
    c->out = out;
    c->user = user;

    if (!c->cctx || !c->inbuf || !c->outbuf) {
        compress_free(c);
        return NULL;
    }

    ZSTD_CCtx_setParameter(c->cctx, ZSTD_c_compressionLevel, level);
    if (threads > 0) {
        ZSTD_CCtx_setParameter(c->cctx, ZSTD_c_nbWorkers, threads);
    }
    if (window_log > 0) {
        ZSTD_CCtx_setParameter(c->cctx, ZSTD_c_windowLog, window_log);
    }
    ZSTD_CCtx_setParameter(c->cctx, ZSTD_c_checksumFlag, 1);
    return c;
}

static int
flush_chunk(compress_ctx_t *c, int final) {
    size_t comp_len;

    comp_len = ZSTD_compress2(c->cctx, c->outbuf, c->outcap, c->inbuf, c->fill);
    if (ZSTD_isError(comp_len)) {
        return -1;
    }
    if (c->out(c->user, c->outbuf, comp_len, c->fill, final)) {
        return -1;
    }
    c->fill = 0;
    return 0;
}

int
compress_write(compress_ctx_t *c, const void *buf, size_t len) {
    const uint8_t *p = buf;

    while (len > 0) {
        size_t room = c->chunk_raw - c->fill;
        size_t take = len < room ? len : room;

        memcpy(c->inbuf + c->fill, p, take);
        c->fill += take;
        p += take;
        len -= take;

        if (c->fill == c->chunk_raw) {
            if (flush_chunk(c, 0)) {
                return -1;
            }
        }
    }
    return 0;
}

int
compress_finish(compress_ctx_t *c) {
    return flush_chunk(c, 1);
}

size_t
compress_buffered(const compress_ctx_t *c) {
    return c->fill;
}

void
compress_free(compress_ctx_t *c) {
    if (!c) {
        return;
    }
    if (c->cctx) {
        ZSTD_freeCCtx(c->cctx);
    }
    free(c->inbuf);
    free(c->outbuf);
    free(c);
}
