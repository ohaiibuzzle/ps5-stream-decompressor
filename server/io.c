/* io.c -- exact-length read/write helpers. */
#include <errno.h>
#include <unistd.h>

#include "io.h"

int
io_nread(int fd, void *buf, size_t n) {
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
            return -1; /* premature EOF */
        }
        off += (size_t)r;
    }
    return 0;
}

int
io_nwrite(int fd, const void *buf, size_t n) {
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
