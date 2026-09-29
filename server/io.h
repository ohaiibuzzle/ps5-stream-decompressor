/* io.h -- exact-length read/write helpers. */
#ifndef PS5SD_IO_H
#define PS5SD_IO_H

#include <stddef.h>
#include <stdint.h>

/* Read exactly n bytes. Returns 0 on success, -1 on error or premature EOF. */
int io_nread(int fd, void *buf, size_t n);

/* Write exactly n bytes. Returns 0 on success, -1 on error. */
int io_nwrite(int fd, const void *buf, size_t n);

#endif /* PS5SD_IO_H */
