/* main-linux.c -- host build of the server, used for integration tests. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "protocol.h"
#include "stream_server.h"

static void
usage(const char *argv0) {
    printf("usage: %s [-p PORT] [-d DIR] [-t TOKEN] [-1] [-q]\n", argv0);
    printf("\n");
    printf("options:\n");
    printf("    -p PORT    listen port (default %d)\n", PS5SD_DEFAULT_PORT);
    printf("    -d DIR     destination directory (default %s)\n",
           PS5SD_DEFAULT_DIR);
    printf("    -t TOKEN   shared secret required from clients (default none)\n");
    printf("    -1         serve a single connection then exit\n");
    printf("    -q         quiet\n");
}

int
main(int argc, char **argv) {
    const char *dir = PS5SD_DEFAULT_DIR;
    uint16_t port = PS5SD_DEFAULT_PORT;
    uint32_t token = 0;
    int once = 0, quiet = 0;
    int c;

    while ((c = getopt(argc, argv, "p:d:t:1qh")) != -1) {
        switch (c) {
        case 'p':
            port = (uint16_t)atoi(optarg);
            break;
        case 'd':
            dir = optarg;
            break;
        case 't':
            token = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case '1':
            once = 1;
            break;
        case 'q':
            quiet = 1;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    return ps5sd_serve(port, dir, token, once, quiet) == 0 ? 0 : 1;
}
