/* stream_server.h -- portable streaming upload server core. */
#ifndef PS5SD_STREAM_SERVER_H
#define PS5SD_STREAM_SERVER_H

#include <stdint.h>

/* Bind `port`, accept connections and decode uploads into `dir`.
 *
 *   port   TCP port to listen on
 *   dir    destination directory (created if missing)
 *   token  shared secret required in HELLO (0 = no auth)
 *   once   accept a single connection then return (used by tests)
 *   quiet  suppress per-transfer logging
 *
 * Returns 0 when `once` is set and a connection was served, -1 on fatal error.
 */
int ps5sd_serve(uint16_t port, const char *dir, uint32_t token, int once,
                int quiet);

#endif /* PS5SD_STREAM_SERVER_H */
