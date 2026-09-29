/* transport.c -- TCP connection and protocol framing (client side). */
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "protocol.h"
#include "transport.h"

static int
nwrite(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    size_t off = 0;

    while (off < n) {
        ssize_t r = write(fd, p + off, n - off);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)r;
    }
    return 0;
}

static int
nread(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    size_t off = 0;

    while (off < n) {
        ssize_t r = read(fd, p + off, n - off);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (r == 0) {
            return -1;
        }
        off += (size_t)r;
    }
    return 0;
}

int
transport_connect(const char *host, uint16_t port) {
    struct addrinfo hints, *res, *rp;
    char service[16];
    int fd = -1;

    snprintf(service, sizeof(service), "%u", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, service, &hints, &res) != 0) {
        return -1;
    }
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int
send_hello_common(int fd, uint8_t codec, uint8_t flags, const char *name,
                  uint64_t raw_size, uint64_t payload_size,
                  uint32_t chunk_raw_size, const uint8_t *props,
                  uint16_t props_len, uint64_t resume_offset,
                  uint32_t token) {
    uint8_t fixed[PS5SD_HELLO_FIXED];
    size_t name_len = strlen(name);

    if (name_len == 0 || name_len > PS5SD_NAME_MAX) {
        return -1;
    }
    ps5sd_hello_serialize(fixed, PS5SD_VERSION, codec, flags,
                          (uint16_t)name_len, props_len, chunk_raw_size, token,
                          resume_offset, raw_size, payload_size);
    if (nwrite(fd, fixed, sizeof(fixed))) {
        return -1;
    }
    if (nwrite(fd, name, name_len)) {
        return -1;
    }
    if (props_len && nwrite(fd, props, props_len)) {
        return -1;
    }
    return 0;
}

int
transport_send_hello(int fd, uint8_t codec, uint8_t flags, const char *name,
                     uint64_t raw_size, uint64_t payload_size,
                     uint32_t chunk_raw_size, const uint8_t *props,
                     uint16_t props_len, uint64_t resume_offset,
                     uint32_t token) {
    return send_hello_common(fd, codec, flags, name, raw_size, payload_size,
                             chunk_raw_size, props, props_len, resume_offset,
                             token);
}

int
transport_query(int fd, const char *name, uint32_t token, uint64_t *size) {
    uint8_t type;
    uint64_t received, written;
    uint32_t code;

    if (send_hello_common(fd, PS5SD_CODEC_RAW, PS5SD_FLAG_QUERY, name, 0, 0, 0,
                          NULL, 0, 0, token)) {
        return -1;
    }
    if (transport_recv_status(fd, &type, &received, &written, &code)) {
        return -1;
    }
    if (type != PS5SD_STATUS_DONE) {
        return -1;
    }
    *size = written;
    return 0;
}

int
transport_send_frame(int fd, const void *buf, uint32_t comp_len,
                     uint32_t raw_len, uint8_t flags) {
    uint8_t h[PS5SD_FRAME_HEADER];

    ps5sd_frame_serialize(h, comp_len, raw_len, flags);
    if (nwrite(fd, h, sizeof(h))) {
        return -1;
    }
    if (comp_len && nwrite(fd, buf, comp_len)) {
        return -1;
    }
    return 0;
}

int
transport_recv_status(int fd, uint8_t *type, uint64_t *received,
                      uint64_t *written, uint32_t *code) {
    uint8_t b[PS5SD_STATUS_LEN];

    if (nread(fd, b, sizeof(b))) {
        return -1;
    }
    ps5sd_status_parse(b, type, received, written, code);
    return 0;
}
