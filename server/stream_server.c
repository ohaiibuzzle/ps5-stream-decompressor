/* stream_server.c -- portable streaming upload server core.
 *
 * The server decodes each frame as it arrives and writes the result straight
 * into the destination file. It never buffers the whole payload/archive, and
 * (apart from resuming an in-place partial file) never uses temporary storage.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "io.h"
#include "decode.h"
#include "protocol.h"
#include "stream_server.h"

/* Largest single frame we are willing to allocate for. */
#define MAX_FRAME (64u * 1024 * 1024)
#define SOCK_BUF  (4u * 1024 * 1024)

typedef struct {
    int fd;
    uint64_t written;
} write_ctx_t;

static int
sink_write(void *user, const void *buf, size_t len) {
    write_ctx_t *w = user;

    if (io_nwrite(w->fd, buf, len)) {
        return -1;
    }
    w->written += len;
    return 0;
}

static int
send_status(int fd, uint8_t type, uint64_t received, uint64_t written,
            uint32_t code) {
    uint8_t b[PS5SD_STATUS_LEN];

    ps5sd_status_serialize(b, type, received, written, code);
    return io_nwrite(fd, b, sizeof(b));
}

/* Create every missing component of `path` (like mkdir -p). */
static int
mkdir_p(const char *path, mode_t mode) {
    char tmp[4096];
    size_t len;

    len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, path, len + 1);
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = 0;
        if (mkdir(tmp, mode) && errno != EEXIST) {
            return -1;
        }
        *p = '/';
    }
    if (mkdir(tmp, mode) && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/* Reject anything that could escape the destination directory. `name` is
 * modified in place to the sanitized value. Returns 0 on success. */
static int
sanitize_name(char *name, size_t len) {
    if (len == 0) {
        return -1;
    }
    if ((len == 1 && name[0] == '.') ||
        (len == 2 && name[0] == '.' && name[1] == '.')) {
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '/' || c == '\\' || c < 0x20 || c == 0x7f) {
            return -1;
        }
    }
    return 0;
}

static int
build_path(const char *dir, const char *name, char *out, size_t outsz) {
    int n = snprintf(out, outsz, "%s/%s", dir, name);

    if (n < 0 || (size_t)n >= outsz) {
        return -1;
    }
    return 0;
}

/* Read and parse the HELLO message. On success fills `h`, plus caller-provided
 * `name` (NUL-terminated) and `props` buffers. */
static int
recv_hello(int fd, ps5sd_hello_t *h, char *name, size_t name_sz,
           uint8_t *props, size_t props_sz) {
    uint8_t fixed[PS5SD_HELLO_FIXED];

    if (io_nread(fd, fixed, sizeof(fixed))) {
        return -1;
    }
    if (ps5sd_hello_parse_fixed(fixed, h)) {
        return -1;
    }
    if (h->name_len == 0 || (size_t)h->name_len >= name_sz) {
        return -1;
    }
    if ((size_t)h->props_len > props_sz) {
        return -1;
    }
    if (io_nread(fd, name, h->name_len)) {
        return -1;
    }
    name[h->name_len] = 0;
    if (h->props_len && io_nread(fd, props, h->props_len)) {
        return -1;
    }
    return 0;
}

static int
handle_connection(int cfd, const char *dir, uint32_t token, int quiet) {
    ps5sd_hello_t h;
    char name[PS5SD_NAME_MAX + 1];
    uint8_t props[PS5SD_PROPS_MAX];
    char path[4096];
    ps5sd_decoder_t *dec = NULL;
    write_ctx_t wctx;
    int fd = -1;
    int rc = -1;
    int remove_on_error = 0;
    uint64_t received = 0;

    if (recv_hello(cfd, &h, name, sizeof(name), props, sizeof(props))) {
        send_status(cfd, PS5SD_STATUS_ERROR, 0, 0, PS5SD_ERR_PROTOCOL);
        return -1;
    }
    if (h.version != PS5SD_VERSION) {
        send_status(cfd, PS5SD_STATUS_ERROR, 0, 0, PS5SD_ERR_BAD_VERSION);
        return -1;
    }

    if (sanitize_name(name, h.name_len)) {
        send_status(cfd, PS5SD_STATUS_ERROR, 0, 0, PS5SD_ERR_BAD_NAME);
        return -1;
    }
    if (token && h.token != token) {
        send_status(cfd, PS5SD_STATUS_ERROR, 0, 0, PS5SD_ERR_BAD_TOKEN);
        return -1;
    }

    /* Query: report the size of an in-place partial file, then stop. */
    if (h.flags & PS5SD_FLAG_QUERY) {
        struct stat st;
        uint64_t sz = 0;

        if (build_path(dir, name, path, sizeof(path)) == 0 &&
            stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            sz = (uint64_t)st.st_size;
        }
        send_status(cfd, PS5SD_STATUS_DONE, 0, sz, PS5SD_OK);
        return 0;
    }

    if (h.codec != PS5SD_CODEC_RAW && h.codec != PS5SD_CODEC_ZSTD &&
        h.codec != PS5SD_CODEC_LZMA2 && h.codec != PS5SD_CODEC_LZMA1) {
        send_status(cfd, PS5SD_STATUS_ERROR, 0, 0, PS5SD_ERR_BAD_CODEC);
        return -1;
    }
    if (mkdir_p(dir, 0755)) {
        perror("mkdir");
        send_status(cfd, PS5SD_STATUS_ERROR, 0, 0, PS5SD_ERR_IO);
        return -1;
    }
    if (build_path(dir, name, path, sizeof(path))) {
        send_status(cfd, PS5SD_STATUS_ERROR, 0, 0, PS5SD_ERR_BAD_NAME);
        return -1;
    }

    if ((h.flags & PS5SD_FLAG_CHUNKED) && h.resume_offset > 0 &&
        h.codec == PS5SD_CODEC_ZSTD) {
        /* Resume: truncate the in-place partial file to the agreed offset and
         * append from there. No temporary file is used. */
        fd = open(path, O_RDWR);
        if (fd < 0 || ftruncate(fd, (off_t)h.resume_offset) ||
            lseek(fd, (off_t)h.resume_offset, SEEK_SET) < 0) {
            perror("resume");
            if (fd >= 0) {
                close(fd);
            }
            send_status(cfd, PS5SD_STATUS_ERROR, 0, 0, PS5SD_ERR_IO);
            return -1;
        }
        wctx.written = h.resume_offset;
    } else {
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror(path);
            send_status(cfd, PS5SD_STATUS_ERROR, 0, 0, PS5SD_ERR_IO);
            return -1;
        }
        wctx.written = 0;
    }
    wctx.fd = fd;

    if (h.codec != PS5SD_CODEC_RAW) {
        dec = ps5sd_decoder_open(h.codec, props, h.props_len, h.raw_size);
        if (!dec) {
            send_status(cfd, PS5SD_STATUS_ERROR, 0, 0, PS5SD_ERR_DECODE);
            remove_on_error = 1;
            goto out;
        }
    }

    if (!quiet) {
        fprintf(stderr, "receiving %s (codec %u, resume %llu)\n", name,
                h.codec, (unsigned long long)h.resume_offset);
    }

    for (;;) {
        uint8_t fh[PS5SD_FRAME_HEADER];
        uint32_t comp_len, raw_len;
        uint8_t flags;
        uint8_t *buf = NULL;

        if (io_nread(cfd, fh, sizeof(fh))) {
            goto out;
        }
        ps5sd_frame_parse(fh, &comp_len, &raw_len, &flags);
        (void)raw_len;

        if (flags == PS5SD_FRAME_ABORT) {
            remove_on_error = 1;
            goto out;
        }
        if (comp_len > MAX_FRAME) {
            send_status(cfd, PS5SD_STATUS_ERROR, received, wctx.written,
                        PS5SD_ERR_PROTOCOL);
            goto out;
        }
        if (comp_len) {
            buf = malloc(comp_len);
            if (!buf || io_nread(cfd, buf, comp_len)) {
                free(buf);
                goto out;
            }
            received += comp_len;

            if (h.codec == PS5SD_CODEC_RAW || flags == PS5SD_FRAME_RAW) {
                if (io_nwrite(fd, buf, comp_len)) {
                    free(buf);
                    goto out;
                }
                wctx.written += comp_len;
            } else if (dec->feed(dec->state, buf, comp_len,
                                 flags == PS5SD_FRAME_FINAL, sink_write,
                                 &wctx)) {
                free(buf);
                send_status(cfd, PS5SD_STATUS_ERROR, received, wctx.written,
                            PS5SD_ERR_DECODE);
                remove_on_error = 1;
                goto out;
            }
            free(buf);
        }
        if (flags == PS5SD_FRAME_FINAL) {
            break;
        }
    }

    if (fsync(fd)) {
        perror("fsync");
    }
    if (!quiet) {
        fprintf(stderr, "wrote %s (%llu bytes)\n", name,
                (unsigned long long)wctx.written);
    }
    send_status(cfd, PS5SD_STATUS_DONE, received, wctx.written, PS5SD_OK);
    rc = 0;

out:
    if (dec) {
        ps5sd_decoder_free(dec);
    }
    if (fd >= 0) {
        close(fd);
    }
    if (remove_on_error && rc != 0) {
        remove(path);
    }
    return rc;
}

int
ps5sd_serve(uint16_t port, const char *dir, uint32_t token, int once,
            int quiet) {
    struct sockaddr_in addr;
    int srvfd;
    int rc = -1;

    signal(SIGPIPE, SIG_IGN);

    srvfd = socket(AF_INET, SOCK_STREAM, 0);
    if (srvfd < 0) {
        perror("socket");
        return -1;
    }

    if (setsockopt(srvfd, SOL_SOCKET, SO_REUSEADDR, &(int){ 1 },
                   sizeof(int)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(srvfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(srvfd);
        return -1;
    }
    if (listen(srvfd, SOMAXCONN) != 0) {
        perror("listen");
        close(srvfd);
        return -1;
    }

    if (!quiet) {
        fprintf(stderr, "listening on port %u, writing to %s\n", port, dir);
    }

    for (;;) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int cfd = accept(srvfd, (struct sockaddr *)&cli, &cli_len);

        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        setsockopt(cfd, SOL_SOCKET, SO_RCVBUF, &(int){ SOCK_BUF },
                   sizeof(int));
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &(int){ 1 }, sizeof(int));

        handle_connection(cfd, dir, token, quiet);
        close(cfd);

        if (once) {
            rc = 0;
            break;
        }
    }

    close(srvfd);
    return rc;
}
