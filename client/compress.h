/* compress.h -- parallel, resumable zstd encoder used by the client.
 *
 * Input is split into independent frames (one per `chunk_raw` bytes). Each
 * frame is compressed by a worker thread, and completed frames are emitted in
 * order by the calling thread. Because frames are independent, the work is
 * embarrassingly parallel and the resulting stream can be resumed at any frame
 * boundary.
 */
#ifndef PS5SD_COMPRESS_H
#define PS5SD_COMPRESS_H

#include <stddef.h>

/* Called (from the thread invoking compress_write/finish) for each compressed
 * frame, in order. `final` marks the last frame. Return non-zero to abort. */
typedef int (*compress_out_fn)(void *user, const void *buf, size_t comp_len,
                               size_t raw_len, int final);

typedef struct compress_ctx compress_ctx_t;

/* Create a frame-pool encoder.
 *
 *   level       zstd compression level
 *   workers     number of worker threads (<=0 means 1)
 *   window_log  zstd windowLog (0 = library default)
 *   chunk_raw   raw bytes per independent frame
 */
compress_ctx_t *compress_new(int level, int workers, int window_log,
                             size_t chunk_raw, compress_out_fn out, void *user);

/* Feed up to `len` raw bytes. May block for backpressure while draining
 * completed frames. Returns 0 on success, -1 on error. */
int compress_write(compress_ctx_t *c, const void *buf, size_t len);

/* Flush the final frame, wait for all workers, and emit everything. */
int compress_finish(compress_ctx_t *c);

void compress_free(compress_ctx_t *c);

/* Size currently buffered in the partial frame. */
size_t compress_buffered(const compress_ctx_t *c);

#endif /* PS5SD_COMPRESS_H */
