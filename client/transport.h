/* transport.h -- TCP connection and protocol framing (client side). */
#ifndef PS5SD_TRANSPORT_H
#define PS5SD_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

/* Connect to host:port. Returns a socket fd, or -1 on error. */
int transport_connect(const char *host, uint16_t port);

/* Ask the server for the size of an existing partial file. Returns 0 on
 * success and stores the byte count in *size. */
int transport_query(int fd, const char *name, uint32_t token, uint64_t *size);

/* Send a HELLO message. Returns 0 on success. */
int transport_send_hello(int fd, uint8_t codec, uint8_t flags,
                         const char *name, uint64_t raw_size,
                         uint64_t payload_size, uint32_t chunk_raw_size,
                         const uint8_t *props, uint16_t props_len,
                         uint64_t resume_offset, uint32_t token);

/* Send one DATA frame. Returns 0 on success. */
int transport_send_frame(int fd, const void *buf, uint32_t comp_len,
                         uint32_t raw_len, uint8_t flags);

/* Receive the server's final STATUS. Returns 0 on success. */
int transport_recv_status(int fd, uint8_t *type, uint64_t *received,
                          uint64_t *written, uint32_t *code);

#endif /* PS5SD_TRANSPORT_H */
