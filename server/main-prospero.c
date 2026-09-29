/* main-prospero.c -- PS5 payload entry point.
 *
 * Builds with the ps5-payload-dev SDK; run the resulting ELF with an ELF
 * loader such as elfldr or websrv.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/syscall.h>

#include <ps5/kernel.h>

#include "protocol.h"
#include "stream_server.h"

#ifndef SYS_thr_set_name
#define SYS_thr_set_name 615
#endif

static void
usage(const char *argv0) {
    printf("usage: %s [-p PORT] [-d DIR] [-t TOKEN] [-q]\n", argv0);
    printf("\n");
    printf("options:\n");
    printf("    -p PORT    listen port (default %d)\n", PS5SD_DEFAULT_PORT);
    printf("    -d DIR     destination directory (default %s)\n",
           PS5SD_DEFAULT_DIR);
    printf("    -t TOKEN   shared secret required from clients (default none)\n");
    printf("    -q         quiet\n");
}

int
main(int argc, char **argv) {
    const char *dir = PS5SD_DEFAULT_DIR;
    uint16_t port = PS5SD_DEFAULT_PORT;
    uint32_t token = 0;
    int quiet = 0;
    int c;

    while ((c = getopt(argc, argv, "p:d:t:qh")) != -1) {
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

    syscall(SYS_thr_set_name, -1, "ps5streamd.elf");
    signal(SIGPIPE, SIG_IGN);

    /* Broaden credentials so the payload can write under /data regardless of
     * the launching process, mirroring other PS5 payloads. */
    if (kernel_set_ucred_authid(getpid(), 0x4801000000000013L)) {
        fprintf(stderr, "warning: unable to change AuthID\n");
    }

    /* The core only returns on a fatal error. */
    ps5sd_serve(port, dir, token, 0, quiet);
    return 0;
}
