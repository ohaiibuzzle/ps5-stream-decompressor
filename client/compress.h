/* compress.h -- streaming zstd encoder used by the client. */
#ifndef PS5SD_COMPRESS_H
#define PS5SD_COMPRESS_H

#include <stddef.h>

/* Called for each compressed frame. `final` marks the last frame. */
typedef int (*compress_out_fn)(void *user, const void *buf, size_t comp_len,
                               size_t raw_len, int final);

typedef struct compress_ctx compress_ctx_t;

/* Create a chunked zstd encoder. Each `chunk_raw` input bytes become one
 * independent frame, which is what makes resuming at chunk boundaries
 * possible.
 *
 *   level       zstd compression level
 *   threads     zstd worker threads (0 = single-threaded)
 *   window_log  zstd windowLog (0 = library default)
 *   chunk_raw   raw bytes per frame
 */
compress_ctx_t *compress_new(int level, int threads, int window_log,
                             size_t chunk_raw, compress_out_fn out, void *user);

/* Feed up to `len` raw bytes. Returns 0 on success, -1 on error. */
int compress_write(compress_ctx_t *c, const void *buf, size_t len);

/* Flush the final frame (always emits at least one frame). */
int compress_finish(compress_ctx_t *c);

void compress_free(compress_ctx_t *c);

/* Size currently buffered but not yet emitted. */
size_t compress_buffered(const compress_ctx_t *c);

#endif /* PS5SD_COMPRESS_H */
