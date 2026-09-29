/* main.c -- ps5push: stream a file (or a single archive member) to the PS5. */
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "archive.h"
#include "compress.h"
#include "protocol.h"
#include "sevenzip.h"
#include "transport.h"

#define DEFAULT_CHUNK (16u * 1024 * 1024)
#define IO_BUFSIZE    (1u * 1024 * 1024)

enum {
    OPT_RAW = 256,
    OPT_NO_PASS,
    OPT_RESUME,
};

typedef struct {
    const char *input;
    const char *host;
    const char *dest;
    const char *member;
    uint16_t port;
    int level;
    int threads;
    int window_log;
    size_t chunk;
    uint32_t token;
    int raw;
    int no_passthrough;
    int resume;
    int yes;
    int quiet;
} options_t;

typedef struct {
    int fd;
    uint64_t wire;
    int error;
} send_ctx_t;typedef struct {
    int is_member;
    FILE *fp;
    member_reader_t *mr;
} source_t;

static const char *
path_basename(const char *p) {
    const char *slash = strrchr(p, '/');
    const char *bslash = strrchr(p, '\\');
    const char *b = p;

    if (slash) b = slash + 1;
    if (bslash && bslash + 1 > b) b = bslash + 1;
    return b;
}

static void
human(uint64_t bytes, char *out, size_t outsz) {
    static const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)bytes;
    int i = 0;

    while (v >= 1024.0 && i < 4) {
        v /= 1024.0;
        i++;
    }
    snprintf(out, outsz, "%.2f %s", v, u[i]);
}

static double
now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static ssize_t
source_read(source_t *s, void *buf, size_t len) {
    if (s->fp) {
        return (ssize_t)fread(buf, 1, len, s->fp);
    }
    return member_reader_read(s->mr, buf, len);
}

static int
on_frame(void *user, const void *buf, size_t comp_len, size_t raw_len,
         int final) {
    send_ctx_t *c = user;
    uint8_t flags = final ? PS5SD_FRAME_FINAL : PS5SD_FRAME_DATA;

    (void)raw_len;
    if (transport_send_frame(c->fd, buf, (uint32_t)comp_len,
                             (uint32_t)raw_len, flags)) {
        c->error = 1;
        return -1;
    }
    c->wire += comp_len;
    return 0;
}

static void
send_abort(int fd) {
    transport_send_frame(fd, NULL, 0, 0, PS5SD_FRAME_ABORT);
}

static void
usage(const char *argv0) {
    printf("usage: %s -H HOST [options] <input>\n\n", argv0);
    printf("Stream <input> (or a single member of a .7z/.zip archive) to a PS5\n");
    printf("running ps5streamd, compressed on the fly.\n\n");
    printf("options:\n");
    printf("  -H, --host HOST         PS5 address (required)\n");
    printf("  -p, --port PORT         server port (default %d)\n",
           PS5SD_DEFAULT_PORT);
    printf("  -d, --dest NAME         destination filename\n");
    printf("  -m, --member PATH       member to extract from the archive\n");
    printf("  -l, --level N           zstd level (default 19)\n");
    printf("  -T, --threads N         zstd threads, 0 = all cores (default all)\n");
    printf("  -W, --window LOG        zstd window log (default 24)\n");
    printf("  -C, --chunk BYTES       frame size in bytes (default 16 MiB)\n");
    printf("  -t, --token N           shared token\n");
    printf("      --raw               do not compress\n");
    printf("      --no-passthrough    always recompress 7z members\n");
    printf("      --resume            resume a previous transfer\n");
    printf("  -y, --yes               never prompt\n");
    printf("  -q, --quiet             reduce output\n");
    printf("  -h, --help              show this help\n");
}

/* Read exactly `len` bytes, discarding them (used to skip to a resume point). */
static int
source_skip(source_t *s, uint64_t len) {
    char buf[65536];

    while (len > 0) {
        size_t take = len < sizeof(buf) ? (size_t)len : sizeof(buf);
        ssize_t n = source_read(s, buf, take);
        if (n <= 0) {
            return -1;
        }
        len -= (uint64_t)n;
    }
    return 0;
}

int
main(int argc, char **argv) {
    options_t opt = {
        .port = PS5SD_DEFAULT_PORT,
        .level = 19,
        .threads = -1,
        .window_log = 24,
        .chunk = DEFAULT_CHUNK,
    };
    static const struct option longopts[] = {
        { "host", required_argument, 0, 'H' },
        { "port", required_argument, 0, 'p' },
        { "dest", required_argument, 0, 'd' },
        { "member", required_argument, 0, 'm' },
        { "level", required_argument, 0, 'l' },
        { "threads", required_argument, 0, 'T' },
        { "window", required_argument, 0, 'W' },
        { "chunk", required_argument, 0, 'C' },
        { "token", required_argument, 0, 't' },
        { "raw", no_argument, 0, OPT_RAW },
        { "no-passthrough", no_argument, 0, OPT_NO_PASS },
        { "resume", no_argument, 0, OPT_RESUME },
        { "yes", no_argument, 0, 'y' },
        { "quiet", no_argument, 0, 'q' },
        { "help", no_argument, 0, 'h' },
        { 0, 0, 0, 0 },
    };
    src_kind_t kind;
    archive_list_t *list = NULL;
    const archive_entry_t *chosen = NULL;
    char chosen_path[4096] = { 0 };
    char dest_name[PS5SD_NAME_MAX + 1] = { 0 };
    sz_pass_t pass;
    int use_pass = 0;
    int codec;
    uint64_t total, payload = 0;
    uint8_t props[PS5SD_PROPS_MAX];
    size_t props_len = 0;
    int c;
    const char *base;
    source_t src = { 0 };
    uint64_t resume_offset = 0;

    memset(&pass, 0, sizeof(pass));

    while ((c = getopt_long(argc, argv, "H:p:d:m:l:T:W:C:t:yqh", longopts,
                            NULL)) != -1) {
        switch (c) {
        case OPT_RAW: opt.raw = 1; break;
        case OPT_NO_PASS: opt.no_passthrough = 1; break;
        case OPT_RESUME: opt.resume = 1; break;
        case 'H': opt.host = optarg; break;
        case 'p': opt.port = (uint16_t)atoi(optarg); break;
        case 'd': opt.dest = optarg; break;
        case 'm': opt.member = optarg; break;
        case 'l': opt.level = atoi(optarg); break;
        case 'T': opt.threads = atoi(optarg); break;
        case 'W': opt.window_log = atoi(optarg); break;
        case 'C': opt.chunk = (size_t)strtoull(optarg, NULL, 0); break;
        case 't': opt.token = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'y': opt.yes = 1; break;
        case 'q': opt.quiet = 1; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }
    if (optind < argc) {
        opt.input = argv[optind];
    }
    if (!opt.input || !opt.host) {
        usage(argv[0]);
        return 1;
    }
    if (opt.threads < 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        opt.threads = n > 0 ? (int)n : 1;
    }
    if (opt.chunk == 0) {
        opt.chunk = DEFAULT_CHUNK;
    }

    kind = archive_detect(opt.input);
    if (kind == SRC_ERROR) {
        fprintf(stderr, "cannot read %s: %s\n", opt.input, strerror(errno));
        return 1;
    }

    if (archive_is_archive(kind)) {
        list = archive_list_open(opt.input);
        if (!list) {
            fprintf(stderr, "cannot open archive %s\n", opt.input);
            return 1;
        }
        size_t n = archive_list_count(list);
        size_t files = 0, single = 0;

        for (size_t i = 0; i < n; i++) {
            const archive_entry_t *e = archive_list_get(list, i);
            if (!e->is_dir) {
                files++;
                single = i;
            }
        }
        if (files == 0) {
            fprintf(stderr, "archive contains no regular files\n");
            archive_list_free(list);
            return 1;
        }

        if (opt.member) {
            for (size_t i = 0; i < n; i++) {
                const archive_entry_t *e = archive_list_get(list, i);
                if (!e->is_dir && strcmp(e->path, opt.member) == 0) {
                    chosen = e;
                    break;
                }
            }
            if (!chosen) {
                fprintf(stderr, "member not found: %s\n", opt.member);
                archive_list_free(list);
                return 1;
            }
        } else if (files == 1) {
            chosen = archive_list_get(list, single);
        } else if (opt.yes || !isatty(fileno(stdin))) {
            fprintf(stderr, "archive has %zu members; select one with --member\n",
                    files);
            archive_list_free(list);
            return 1;
        } else {
            fprintf(stderr, "Archive %s contains %zu members:\n", opt.input,
                    files);
            for (size_t i = 0; i < n; i++) {
                const archive_entry_t *e = archive_list_get(list, i);
                char h[32];
                if (e->is_dir) continue;
                human(e->size, h, sizeof(h));
                fprintf(stderr, "  [%zu] %10s  %s\n", i, h, e->path);
            }
            fprintf(stderr, "Select member index (empty = send the whole archive): ");
            char line[64];
            if (!fgets(line, sizeof(line), stdin) || line[0] == '\n') {
                chosen = NULL;
            } else {
                long idx = strtol(line, NULL, 10);
                if (idx >= 0 && (size_t)idx < n) {
                    chosen = archive_list_get(list, (size_t)idx);
                }
                if (!chosen || chosen->is_dir) {
                    fprintf(stderr, "invalid selection\n");
                    archive_list_free(list);
                    return 1;
                }
            }
        }

        if (chosen) {
            snprintf(chosen_path, sizeof(chosen_path), "%s", chosen->path);
        }
        /* else: fall through and send the archive file itself. */
    }

    if (chosen) {
        base = path_basename(chosen_path);
        snprintf(dest_name, sizeof(dest_name), "%s", opt.dest ? opt.dest : base);
        total = chosen->size;

        if (kind == SRC_7Z && !opt.no_passthrough && !opt.raw) {
            if (sevenzip_passthrough_info(opt.input, chosen_path, &pass) == 0 &&
                pass.supported) {
                use_pass = 1;
            }
        }
    } else {
        /* Send the input file verbatim. */
        FILE *f = fopen(opt.input, "rb");
        if (!f) {
            fprintf(stderr, "cannot open %s: %s\n", opt.input, strerror(errno));
            archive_list_free(list);
            return 1;
        }
        total = 0;
        if (fseeko(f, 0, SEEK_END) == 0) {
            total = (uint64_t)ftello(f);
        }
        fclose(f);
        base = path_basename(opt.input);
        snprintf(dest_name, sizeof(dest_name), "%s", opt.dest ? opt.dest : base);
        /* The input is already compressed; store it verbatim. */
        if (archive_is_archive(kind)) {
            opt.raw = 1;
        }
    }

    if (use_pass) {
        codec = pass.codec;
        props_len = pass.props_len;
        memcpy(props, pass.props, props_len);
        payload = pass.pack_size;
        total = pass.raw_size;
    } else if (opt.raw) {
        codec = PS5SD_CODEC_RAW;
        payload = total;
    } else {
        codec = PS5SD_CODEC_ZSTD;
    }

    if (!opt.quiet) {
        char h[32];
        human(total, h, sizeof(h));
        fprintf(stderr, "%s -> %s:%u/%s (%s)\n", opt.input, opt.host, opt.port,
                dest_name, h);
        if (chosen) {
            fprintf(stderr, "member: %s\n", chosen_path);
        }
        if (use_pass) {
            fprintf(stderr, "using 7z pass-through (%s, %llu packed bytes)\n",
                    codec == PS5SD_CODEC_LZMA2 ? "LZMA2" : "LZMA1",
                    (unsigned long long)pass.pack_size);
        } else if (codec == PS5SD_CODEC_ZSTD) {
            fprintf(stderr, "recompressing with zstd level %d (%d threads)\n",
                    opt.level, opt.threads);
        } else {
            fprintf(stderr, "sending uncompressed\n");
        }
    }

    /* Open the data source. */
    if (chosen && !use_pass) {
        src.is_member = 1;
        src.mr = member_reader_open(opt.input, chosen_path);
        if (!src.mr) {
            fprintf(stderr, "cannot open member %s\n", chosen_path);
            archive_list_free(list);
            return 1;
        }
    } else {
        src.fp = fopen(opt.input, "rb");
        if (!src.fp) {
            fprintf(stderr, "cannot open %s: %s\n", opt.input, strerror(errno));
            archive_list_free(list);
            return 1;
        }
        if (use_pass && fseeko(src.fp, (off_t)pass.pack_offset, SEEK_SET)) {
            fprintf(stderr, "cannot seek to packed stream: %s\n",
                    strerror(errno));
            fclose(src.fp);
            archive_list_free(list);
            return 1;
        }
    }

    /* Resume: ask the server how much it already has. */
    if (opt.resume && codec == PS5SD_CODEC_ZSTD && !opt.raw) {
        int qfd = transport_connect(opt.host, opt.port);
        uint64_t have = 0;
        if (qfd >= 0) {
            if (transport_query(qfd, dest_name, opt.token, &have) == 0) {
                resume_offset = (have / opt.chunk) * opt.chunk;
                if (resume_offset > total) {
                    resume_offset = total;
                }
            }
            close(qfd);
        }
        if (!opt.quiet && resume_offset) {
            fprintf(stderr, "resuming at %llu bytes\n",
                    (unsigned long long)resume_offset);
        }
    }

    /* Reconnect for the actual transfer. */
    int fd = transport_connect(opt.host, opt.port);
    if (fd < 0) {
        fprintf(stderr, "cannot connect to %s:%u: %s\n", opt.host, opt.port,
                strerror(errno));
        goto fail;
    }

    {
        uint8_t flags = (codec == PS5SD_CODEC_ZSTD)
                            ? PS5SD_FLAG_CHUNKED
                            : 0;
        if (props_len) {
            flags |= PS5SD_FLAG_HAS_PROPS;
        }
        if (transport_send_hello(fd, (uint8_t)codec, flags, dest_name, total,
                                 payload, (uint32_t)opt.chunk, props,
                                 (uint16_t)props_len, resume_offset,
                                 opt.token)) {
            fprintf(stderr, "failed to send handshake\n");
            close(fd);
            goto fail;
        }
    }

    {
        send_ctx_t sctx = { .fd = fd };
        uint8_t *buf = malloc(IO_BUFSIZE);
        uint64_t done = 0;
        double t0 = now_sec(), tlast = t0;
        uint64_t last_report = 0;

        if (!buf) {
            fprintf(stderr, "out of memory\n");
            send_abort(fd);
            close(fd);
            goto fail;
        }

        /* Position the source at the resume point. */
        if (resume_offset) {
            if (src.fp) {
                if (fseeko(src.fp, (off_t)resume_offset, SEEK_SET)) {
                    fprintf(stderr, "cannot seek: %s\n", strerror(errno));
                    free(buf);
                    send_abort(fd);
                    close(fd);
                    goto fail;
                }
            } else if (source_skip(&src, resume_offset)) {
                fprintf(stderr, "cannot skip to resume point\n");
                free(buf);
                send_abort(fd);
                close(fd);
                goto fail;
            }
            done = resume_offset;
        }

        if (codec == PS5SD_CODEC_ZSTD) {
            compress_ctx_t *enc =
                compress_new(opt.level, opt.threads, opt.window_log, opt.chunk,
                             on_frame, &sctx);
            if (!enc) {
                fprintf(stderr, "cannot create compressor\n");
                free(buf);
                send_abort(fd);
                close(fd);
                goto fail;
            }
            while (done < total && !sctx.error) {
                size_t want = total - done < IO_BUFSIZE
                                  ? (size_t)(total - done)
                                  : IO_BUFSIZE;
                ssize_t n = source_read(&src, buf, want);
                if (n < 0) {
                    fprintf(stderr, "read error\n");
                    sctx.error = 1;
                    break;
                }
                if (n == 0) {
                    break;
                }
                if (compress_write(enc, buf, (size_t)n)) {
                    fprintf(stderr, "compression error\n");
                    sctx.error = 1;
                    break;
                }
                done += (uint64_t)n;

                if (!opt.quiet) {
                    double t = now_sec();
                    if (t - tlast >= 1.0 || done == total) {
                        char rh[32], th[32], wh[32];
                        human(done, rh, sizeof(rh));
                        human(total, th, sizeof(th));
                        human(sctx.wire, wh, sizeof(wh));
                        fprintf(stderr,
                                "\r  %s / %s raw, %s sent (%.1f%%)   ",
                                rh, th, wh,
                                total ? 100.0 * (double)done / (double)total
                                      : 100.0);
                        fflush(stderr);
                        tlast = t;
                        last_report = done;
                    }
                }
            }
            (void)last_report;
            if (!sctx.error && compress_finish(enc)) {
                fprintf(stderr, "compression error\n");
                sctx.error = 1;
            }
            compress_free(enc);
        } else {
            /* Direct streaming: raw or 7z packed bytes. */
            uint64_t remaining = use_pass ? payload : total;
            while (remaining > 0 && !sctx.error) {
                size_t want = remaining < IO_BUFSIZE ? (size_t)remaining
                                                     : IO_BUFSIZE;
                ssize_t n = source_read(&src, buf, want);
                if (n <= 0) {
                    fprintf(stderr, "read error\n");
                    sctx.error = 1;
                    break;
                }
                uint64_t left = remaining - (uint64_t)n;
                uint8_t flags = left == 0 ? PS5SD_FRAME_FINAL
                                          : PS5SD_FRAME_DATA;
                if (transport_send_frame(fd, buf, (uint32_t)n, (uint32_t)n,
                                         flags)) {
                    sctx.error = 1;
                    break;
                }
                sctx.wire += (uint64_t)n;
                remaining = left;
                done += (uint64_t)n;
            }
            if (!sctx.error && done == 0) {
                /* Empty input: send an explicit final frame. */
                if (transport_send_frame(fd, buf, 0, 0, PS5SD_FRAME_FINAL)) {
                    sctx.error = 1;
                }
            }
            if (!opt.quiet) {
                fprintf(stderr, "\r  sent %llu bytes                    ",
                        (unsigned long long)done);
            }
        }

        if (!opt.quiet) {
            fprintf(stderr, "\n");
        }
        free(buf);

        if (sctx.error) {
            send_abort(fd);
            close(fd);
            goto fail;
        }

        {
            uint8_t type;
            uint64_t received, written;
            uint32_t code;
            if (transport_recv_status(fd, &type, &received, &written, &code)) {
                fprintf(stderr, "no response from server\n");
                close(fd);
                goto fail;
            }
            if (type != PS5SD_STATUS_DONE) {
                fprintf(stderr, "server error: code %u\n", code);
                close(fd);
                goto fail;
            }
            if (!opt.quiet) {
                char wh[32];
                double dt = now_sec() - t0;
                human(written, wh, sizeof(wh));
                fprintf(stderr, "done: wrote %s in %.1fs (%.2f MiB/s)\n", wh, dt,
                        dt > 0 ? (double)written / dt / 1048576.0 : 0.0);
            }
        }
        close(fd);
    }

    if (src.fp) fclose(src.fp);
    if (src.mr) member_reader_free(src.mr);
    archive_list_free(list);
    return 0;

fail:
    if (src.fp) fclose(src.fp);
    if (src.mr) member_reader_free(src.mr);
    archive_list_free(list);
    return 1;
}
