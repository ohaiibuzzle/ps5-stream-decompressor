/* archive.c -- archive detection, listing and member streaming (libarchive). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <archive.h>
#include <archive_entry.h>
#include "archive.h"

static const unsigned char k_7z_magic[6] = { 0x37, 0x7a, 0xbc, 0xaf,
                                             0x27, 0x1c };

src_kind_t
archive_detect(const char *path) {
    unsigned char buf[8];
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) {
        return SRC_ERROR;
    }
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n >= 6 && memcmp(buf, k_7z_magic, 6) == 0) {
        return SRC_7Z;
    }
    if (n >= 4 && buf[0] == 'P' && buf[1] == 'K' &&
        ((buf[2] == 3 && buf[3] == 4) || (buf[2] == 5 && buf[3] == 6) ||
         (buf[2] == 7 && buf[3] == 8))) {
        return SRC_ZIP;
    }
    return SRC_PLAIN;
}

int
archive_is_archive(src_kind_t kind) {
    return kind == SRC_ZIP || kind == SRC_7Z;
}

struct archive_list {
    archive_entry_t *entries;
    size_t count;
    size_t cap;
};

static int
list_append(archive_list_t *l, const char *path, uint64_t size, int is_dir) {
    archive_entry_t *e;

    if (l->count == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 16;
        void *p = realloc(l->entries, ncap * sizeof(*l->entries));
        if (!p) {
            return -1;
        }
        l->entries = p;
        l->cap = ncap;
    }
    e = &l->entries[l->count];
    e->path = strdup(path);
    if (!e->path) {
        return -1;
    }
    e->size = size;
    e->is_dir = is_dir;
    l->count++;
    return 0;
}

archive_list_t *
archive_list_open(const char *path) {
    struct archive *a;
    struct archive_entry *entry;
    archive_list_t *l;

    a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, path, 1 << 20) != ARCHIVE_OK) {
        archive_read_free(a);
        return NULL;
    }

    l = calloc(1, sizeof(*l));
    if (!l) {
        archive_read_free(a);
        return NULL;
    }

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        mode_t mode = archive_entry_mode(entry);
        int is_dir;
        uint64_t size;

        if (!name) {
            continue;
        }
        is_dir = archive_entry_filetype(entry) == AE_IFDIR ||
                 S_ISDIR(mode) ||
                 (name[0] && name[strlen(name) - 1] == '/');
        /* Directories have no meaningful size. */
        size = is_dir ? 0 : (uint64_t)archive_entry_size(entry);
        if (list_append(l, name, size, is_dir)) {
            archive_list_free(l);
            archive_read_free(a);
            return NULL;
        }
    }
    archive_read_free(a);
    return l;
}

size_t
archive_list_count(const archive_list_t *l) {
    return l->count;
}

const archive_entry_t *
archive_list_get(const archive_list_t *l, size_t i) {
    return i < l->count ? &l->entries[i] : NULL;
}

void
archive_list_free(archive_list_t *l) {
    if (!l) {
        return;
    }
    for (size_t i = 0; i < l->count; i++) {
        free(l->entries[i].path);
    }
    free(l->entries);
    free(l);
}

struct member_reader {
    struct archive *a;
};

member_reader_t *
member_reader_open(const char *path, const char *member) {
    struct archive *a;
    struct archive_entry *entry;
    member_reader_t *r;

    a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, path, 1 << 20) != ARCHIVE_OK) {
        archive_read_free(a);
        return NULL;
    }
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        mode_t mode = archive_entry_mode(entry);
        int is_dir = archive_entry_filetype(entry) == AE_IFDIR ||
                     S_ISDIR(mode) ||
                     (name && name[0] && name[strlen(name) - 1] == '/');
        if (!is_dir && name && strcmp(name, member) == 0) {
            r = calloc(1, sizeof(*r));
            if (!r) {
                archive_read_free(a);
                return NULL;
            }
            r->a = a;
            return r;
        }
    }
    archive_read_free(a);
    return NULL;
}

ssize_t
member_reader_read(member_reader_t *r, void *buf, size_t len) {
    la_ssize_t n = archive_read_data(r->a, buf, len);

    if (n < 0) {
        return -1;
    }
    return (ssize_t)n;
}

void
member_reader_free(member_reader_t *r) {
    if (!r) {
        return;
    }
    if (r->a) {
        archive_read_free(r->a);
    }
    free(r);
}
