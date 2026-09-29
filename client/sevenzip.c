/* sevenzip.c -- 7z pass-through inspection via the 7-Zip SDK. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "7zFile.h"

#include "protocol.h"
#include "sevenzip.h"

#define K_LZMA2 0x21u
#define K_LZMA  0x30101u

static ISzAlloc g_alloc = { SzAlloc, SzFree };
static ISzAlloc g_alloc_temp = { SzAllocTemp, SzFreeTemp };

/* Convert a UTF-16LE string to UTF-8. */
static void
utf16le_to_utf8(const UInt16 *s, char *out, size_t outsz) {
    size_t o = 0;

    for (size_t i = 0; s[i]; i++) {
        uint32_t cp = s[i];

        if (cp >= 0xD800 && cp <= 0xDBFF && s[i + 1] >= 0xDC00 &&
            s[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) {
            if (o + 1 >= outsz) break;
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            if (o + 2 >= outsz) break;
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            if (o + 3 >= outsz) break;
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (o + 4 >= outsz) break;
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = 0;
}

static int
get_name_utf8(const CSzArEx *db, UInt32 index, char *out, size_t outsz) {
    size_t len = SzArEx_GetFileNameUtf16(db, index, NULL);
    UInt16 *u16;
    size_t i;

    if (len == 0) {
        return -1;
    }
    u16 = calloc(len + 1, sizeof(UInt16));
    if (!u16) {
        return -1;
    }
    SzArEx_GetFileNameUtf16(db, index, u16);
    for (i = 0; i <= len; i++) {
        if (i == len || u16[i] == 0) {
            u16[i] = 0;
            break;
        }
    }
    utf16le_to_utf8(u16, out, outsz);
    free(u16);
    return 0;
}

int
sevenzip_passthrough_info(const char *path, const char *member,
                          sz_pass_t *out) {
    CFileInStream fs;
    CLookToRead2 look;
    CSzArEx db;
    Byte *buf = NULL;
    const size_t bufsize = 1 << 16;
    int rc = -1;

    memset(out, 0, sizeof(*out));

    /* 7-Zip needs its CRC table before parsing an archive header. */
    CrcGenerateTable();

    FileInStream_CreateVTable(&fs);
    if (InFile_Open(&fs.file, path) != 0) {
        return -1;
    }
    buf = malloc(bufsize);
    if (!buf) {
        File_Close(&fs.file);
        return -1;
    }
    LookToRead2_CreateVTable(&look, 0);
    look.realStream = &fs.vt;
    look.buf = buf;
    look.bufSize = bufsize;
    LookToRead2_INIT(&look);

    SzArEx_Init(&db);
    if (SzArEx_Open(&db, &look.vt, &g_alloc, &g_alloc_temp) != SZ_OK) {
        goto done;
    }

    for (UInt32 i = 0; i < db.NumFiles; i++) {
        char name[4096];
        UInt32 folder;
        const Byte *coder_data;
        CSzFolder csz;
        CSzData sd;
        UInt32 method;
        UInt32 base;

        if (SzArEx_IsDir(&db, i)) {
            continue;
        }
        if (get_name_utf8(&db, i, name, sizeof(name))) {
            continue;
        }
        if (strcmp(name, member) != 0) {
            continue;
        }

        folder = db.FileToFolder[i];
        if (folder == (UInt32)-1) {
            goto done; /* empty file */
        }
        /* Pass-through only makes sense when the member owns its solid block. */
        if (db.FolderToFile[(size_t)folder + 1] - db.FolderToFile[folder]
            != 1) {
            goto done;
        }

        coder_data = db.db.CodersData + db.db.FoCodersOffsets[folder];
        sd.Data = coder_data;
        sd.Size = db.db.FoCodersOffsets[(size_t)folder + 1] -
                  db.db.FoCodersOffsets[folder];
        if (SzGetNextFolderItem(&csz, &sd) != SZ_OK) {
            goto done;
        }
        if (csz.NumCoders != 1 || csz.NumPackStreams != 1) {
            goto done;
        }
        method = (UInt32)csz.Coders[0].MethodID;
        if (method == K_LZMA2) {
            out->codec = PS5SD_CODEC_LZMA2;
        } else if (method == K_LZMA) {
            out->codec = PS5SD_CODEC_LZMA1;
        } else {
            goto done;
        }
        out->props_len = csz.Coders[0].PropsSize;
        if (out->props_len > sizeof(out->props)) {
            goto done;
        }
        memcpy(out->props,
               coder_data + csz.Coders[0].PropsOffset, out->props_len);

        base = db.db.FoStartPackStreamIndex[folder];
        out->pack_offset = db.dataPos + db.db.PackPositions[base];
        out->pack_size = db.db.PackPositions[base + 1] -
                         db.db.PackPositions[base];
        out->raw_size = SzArEx_GetFileSize(&db, i);
        out->supported = 1;
        rc = 0;
        break;
    }

done:
    SzArEx_Free(&db, &g_alloc);
    free(buf);
    File_Close(&fs.file);
    return rc;
}
