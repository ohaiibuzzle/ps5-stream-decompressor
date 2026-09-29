/* sevenzip.h -- 7z pass-through inspection via the 7-Zip SDK.
 *
 * 7z archives cannot be stream-decoded because their header sits at the end.
 * The client, however, has random access to the local file, so it can parse
 * the header with SzArEx_Open() and hand the PS5 the raw compressed stream of
 * a single member together with the coder properties. The PS5 then decodes it
 * with the matching LZMA11/LZMA2 decoder.
 */
#ifndef PS5SD_SEVENZIP_H
#define PS5SD_SEVENZIP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int supported;      /* 1 if the member can be passed through verbatim */
    int codec;          /* PS5SD_CODEC_LZMA2 / LZMA1 */
    uint8_t props[16];
    size_t props_len;
    uint64_t pack_offset; /* absolute byte offset of the packed stream */
    uint64_t pack_size;   /* packed stream length */
    uint64_t raw_size;    /* uncompressed member size */
} sz_pass_t;

/* Inspect the member `member` (internal path) of the 7z archive at `path`.
 * Returns 0 and fills `out` on success; out->supported tells whether
 * pass-through is possible. Returns -1 on error. */
int sevenzip_passthrough_info(const char *path, const char *member,
                              sz_pass_t *out);

#endif /* PS5SD_SEVENZIP_H */
