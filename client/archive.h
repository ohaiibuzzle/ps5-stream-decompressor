/* archive.h -- archive detection, listing and member streaming (libarchive). */
#ifndef PS5SD_ARCHIVE_H
#define PS5SD_ARCHIVE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef enum {
    SRC_ERROR = -1,
    SRC_PLAIN = 0,
    SRC_ZIP,
    SRC_7Z,
} src_kind_t;

/* Classify a file by magic bytes. Returns SRC_ERROR if it cannot be read. */
src_kind_t archive_detect(const char *path);

/* Return non-zero if `kind` is an archive we can list/extract members from. */
int archive_is_archive(src_kind_t kind);

typedef struct {
    char *path;
    uint64_t size;
    int is_dir;
} archive_entry_t;

typedef struct archive_list archive_list_t;

archive_list_t *archive_list_open(const char *path);
size_t archive_list_count(const archive_list_t *l);
const archive_entry_t *archive_list_get(const archive_list_t *l, size_t i);
void archive_list_free(archive_list_t *l);

/* Streaming reader for a single member, selected by its internal path. */
typedef struct member_reader member_reader_t;

member_reader_t *member_reader_open(const char *path, const char *member);
ssize_t member_reader_read(member_reader_t *r, void *buf, size_t len);
void member_reader_free(member_reader_t *r);

#endif /* PS5SD_ARCHIVE_H */
