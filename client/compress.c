/* compress.c -- parallel, resumable zstd encoder with decoupled output.
 *
 * Input is split into independent frames (one per `chunk_raw` bytes). A pool of
 * worker threads compresses them, and a dedicated sender thread writes
 * completed frames to the network in order. Decoupling the sender from the
 * reader/workers means a slow or bursty network no longer stalls compression:
 * the network is drained continuously while the CPU keeps working, within a
 * bounded amount of buffered frames.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

#include "compress.h"

enum slot_state {
    SLOT_FREE,
    SLOT_FILLING,
    SLOT_READY,
    SLOT_COMPRESSING,
    SLOT_DONE,
    SLOT_SENDING,
};

typedef struct {
    uint64_t seq;
    size_t fill;
    size_t out_len;
    int final;
    enum slot_state state;
    uint8_t *in;
    uint8_t *out;
} slot_t;

struct compress_ctx {
    pthread_t *workers;
    pthread_t sender;
    int sender_started;
    int nworkers;
    slot_t *slots;
    int nslots;
    size_t chunk_raw;
    size_t out_cap;
    int level;
    int window_log;

    compress_out_fn out;
    void *user;

    pthread_mutex_t lock;
    pthread_cond_t ready_cv; /* workers wait for READY slots */
    pthread_cond_t send_cv;  /* sender waits for DONE frames */
    pthread_cond_t free_cv;  /* reader waits for a free slot */
    uint64_t next_fill;
    uint64_t next_send;
    int shutdown;
    int error;
    int joined;
    int inited;
    slot_t *cur; /* partial frame being filled (calling thread only) */
};

/* Emit completed frames in order until none is ready. Runs on the sender
 * thread only. */
static void *
sender_main(void *arg) {
    compress_ctx_t *c = arg;

    for (;;) {
        slot_t *s;
        size_t out_len, raw_len;
        int final, rc;

        pthread_mutex_lock(&c->lock);
        for (;;) {
            if (c->error || c->shutdown) {
                pthread_mutex_unlock(&c->lock);
                return NULL;
            }
            if (c->next_send < c->next_fill) {
                s = &c->slots[c->next_send % (uint64_t)c->nslots];
                if (s->state == SLOT_DONE) {
                    break;
                }
            }
            pthread_cond_wait(&c->send_cv, &c->lock);
        }
        s->state = SLOT_SENDING;
        out_len = s->out_len;
        raw_len = s->fill;
        final = s->final;
        pthread_mutex_unlock(&c->lock);

        rc = c->out(c->user, s->out, out_len, raw_len, final);

        pthread_mutex_lock(&c->lock);
        if (rc) {
            c->error = 1;
        }
        s->state = SLOT_FREE;
        c->next_send++;
        pthread_cond_broadcast(&c->free_cv);
        pthread_cond_broadcast(&c->send_cv);
        pthread_mutex_unlock(&c->lock);

        if (rc) {
            return NULL;
        }
    }
}

/* Reserve the next slot for filling, waiting (with backpressure) while the
 * window is full. Returns a FILLING slot with fill == 0, or NULL on error. */
static slot_t *
acquire_slot(compress_ctx_t *c) {
    slot_t *s;

    pthread_mutex_lock(&c->lock);
    while (!c->error &&
           c->next_fill - c->next_send >= (uint64_t)c->nslots) {
        pthread_cond_wait(&c->free_cv, &c->lock);
    }
    if (c->error) {
        pthread_mutex_unlock(&c->lock);
        return NULL;
    }
    s = &c->slots[c->next_fill % (uint64_t)c->nslots];
    s->seq = c->next_fill;
    s->fill = 0;
    s->out_len = 0;
    s->final = 0;
    s->state = SLOT_FILLING;
    c->next_fill++;
    pthread_mutex_unlock(&c->lock);
    return s;
}

static void
submit_slot(compress_ctx_t *c, slot_t *s) {
    pthread_mutex_lock(&c->lock);
    s->state = SLOT_READY;
    pthread_cond_broadcast(&c->ready_cv);
    pthread_mutex_unlock(&c->lock);
}

static void *
worker_main(void *arg) {
    compress_ctx_t *c = arg;
    ZSTD_CCtx *z = ZSTD_createCCtx();

    if (z) {
        ZSTD_CCtx_setParameter(z, ZSTD_c_compressionLevel, c->level);
        if (c->window_log > 0) {
            ZSTD_CCtx_setParameter(z, ZSTD_c_windowLog, c->window_log);
        }
        ZSTD_CCtx_setParameter(z, ZSTD_c_checksumFlag, 1);
    }

    for (;;) {
        slot_t *s = NULL;
        uint8_t *in;
        size_t fill;
        size_t cl;
        int err;

        pthread_mutex_lock(&c->lock);
        for (;;) {
            if (c->error) {
                pthread_mutex_unlock(&c->lock);
                ZSTD_freeCCtx(z);
                return NULL;
            }
            for (int i = 0; i < c->nslots; i++) {
                if (c->slots[i].state == SLOT_READY) {
                    s = &c->slots[i];
                    break;
                }
            }
            if (s) {
                break;
            }
            if (c->shutdown) {
                pthread_mutex_unlock(&c->lock);
                ZSTD_freeCCtx(z);
                return NULL;
            }
            pthread_cond_wait(&c->ready_cv, &c->lock);
        }
        s->state = SLOT_COMPRESSING;
        in = s->in;
        fill = s->fill;
        pthread_mutex_unlock(&c->lock);

        cl = z ? ZSTD_compress2(z, s->out, c->out_cap, in, fill) : 0;
        err = (!z || ZSTD_isError(cl));

        pthread_mutex_lock(&c->lock);
        if (err) {
            c->error = 1;
            pthread_cond_broadcast(&c->ready_cv);
            pthread_cond_broadcast(&c->send_cv);
            pthread_cond_broadcast(&c->free_cv);
            pthread_mutex_unlock(&c->lock);
            ZSTD_freeCCtx(z);
            return NULL;
        }
        s->out_len = cl;
        s->state = SLOT_DONE;
        pthread_cond_broadcast(&c->send_cv);
        pthread_mutex_unlock(&c->lock);
    }
}

compress_ctx_t *
compress_new(int level, int workers, int window_log, size_t chunk_raw,
             size_t buffer_bytes, compress_out_fn out, void *user) {
    compress_ctx_t *c;

    if (chunk_raw == 0) {
        return NULL;
    }
    if (workers < 1) {
        workers = 1;
    }

    c = calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->nworkers = workers;
    c->chunk_raw = chunk_raw;
    c->out_cap = ZSTD_compressBound(chunk_raw);
    c->level = level;
    c->window_log = window_log;
    c->out = out;
    c->user = user;

    /* Choose the number of in-flight slots from the buffer budget. Each slot
     * holds one input and one output buffer, so this bounds memory. Always
     * keep at least one slot per worker plus headroom. */
    {
        uint64_t slot_bytes = (uint64_t)chunk_raw + c->out_cap;
        int slots = slot_bytes ? (int)(buffer_bytes / slot_bytes) : workers + 2;

        if (slots < workers + 2) {
            slots = workers + 2;
        }
        if (slots > 4096) {
            slots = 4096;
        }
        c->nslots = slots;
    }

    c->workers = calloc((size_t)c->nworkers, sizeof(pthread_t));
    c->slots = calloc((size_t)c->nslots, sizeof(slot_t));
    if (!c->workers || !c->slots) {
        compress_free(c);
        return NULL;
    }
    for (int i = 0; i < c->nslots; i++) {
        c->slots[i].state = SLOT_FREE;
        c->slots[i].in = malloc(chunk_raw);
        c->slots[i].out = malloc(c->out_cap);
        if (!c->slots[i].in || !c->slots[i].out) {
            compress_free(c);
            return NULL;
        }
    }

    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->ready_cv, NULL);
    pthread_cond_init(&c->send_cv, NULL);
    pthread_cond_init(&c->free_cv, NULL);
    c->inited = 1;

    if (pthread_create(&c->sender, NULL, sender_main, c) != 0) {
        compress_free(c);
        return NULL;
    }
    c->sender_started = 1;

    for (int i = 0; i < c->nworkers; i++) {
        if (pthread_create(&c->workers[i], NULL, worker_main, c) != 0) {
            pthread_mutex_lock(&c->lock);
            c->error = 1;
            c->shutdown = 1;
            pthread_cond_broadcast(&c->ready_cv);
            pthread_cond_broadcast(&c->send_cv);
            pthread_cond_broadcast(&c->free_cv);
            pthread_mutex_unlock(&c->lock);
            for (int j = 0; j < i; j++) {
                pthread_join(c->workers[j], NULL);
            }
            if (c->sender_started) {
                pthread_join(c->sender, NULL);
                c->sender_started = 0;
            }
            c->joined = 1;
            compress_free(c);
            return NULL;
        }
    }
    return c;
}

int
compress_write(compress_ctx_t *c, const void *buf, size_t len) {
    const uint8_t *p = buf;

    if (c->error) {
        return -1;
    }
    while (len > 0) {
        size_t room, take;

        if (!c->cur) {
            c->cur = acquire_slot(c);
            if (!c->cur) {
                return -1;
            }
        }
        room = c->chunk_raw - c->cur->fill;
        take = len < room ? len : room;
        memcpy(c->cur->in + c->cur->fill, p, take);
        c->cur->fill += take;
        p += take;
        len -= take;

        if (c->cur->fill == c->chunk_raw) {
            slot_t *s = c->cur;
            c->cur = NULL;
            submit_slot(c, s);
        }
    }
    return 0;
}

int
compress_finish(compress_ctx_t *c) {
    if (!c->cur) {
        c->cur = acquire_slot(c);
        if (!c->cur) {
            return -1;
        }
    }
    {
        slot_t *s = c->cur;
        c->cur = NULL;
        s->final = 1;
        submit_slot(c, s);
    }

    /* Wait for the sender to drain everything. */
    pthread_mutex_lock(&c->lock);
    while (!c->error && c->next_send != c->next_fill) {
        pthread_cond_wait(&c->free_cv, &c->lock);
    }
    pthread_mutex_unlock(&c->lock);

    if (!c->joined) {
        pthread_mutex_lock(&c->lock);
        c->shutdown = 1;
        pthread_cond_broadcast(&c->ready_cv);
        pthread_cond_broadcast(&c->send_cv);
        pthread_cond_broadcast(&c->free_cv);
        pthread_mutex_unlock(&c->lock);
        for (int i = 0; i < c->nworkers; i++) {
            pthread_join(c->workers[i], NULL);
        }
        if (c->sender_started) {
            pthread_join(c->sender, NULL);
            c->sender_started = 0;
        }
        c->joined = 1;
    }
    return c->error ? -1 : 0;
}

size_t
compress_buffered(const compress_ctx_t *c) {
    return c->cur ? c->cur->fill : 0;
}

int
compress_workers(const compress_ctx_t *c) {
    return c->nworkers;
}

int
compress_slots(const compress_ctx_t *c) {
    return c->nslots;
}

void
compress_free(compress_ctx_t *c) {
    if (!c) {
        return;
    }
    if (!c->joined && c->inited && c->slots) {
        pthread_mutex_lock(&c->lock);
        c->shutdown = 1;
        pthread_cond_broadcast(&c->ready_cv);
        pthread_cond_broadcast(&c->send_cv);
        pthread_cond_broadcast(&c->free_cv);
        pthread_mutex_unlock(&c->lock);
        for (int i = 0; i < c->nworkers; i++) {
            pthread_join(c->workers[i], NULL);
        }
        if (c->sender_started) {
            pthread_join(c->sender, NULL);
            c->sender_started = 0;
        }
        c->joined = 1;
    }
    if (c->slots) {
        for (int i = 0; i < c->nslots; i++) {
            free(c->slots[i].in);
            free(c->slots[i].out);
        }
    }
    free(c->slots);
    free(c->workers);
    if (c->inited) {
        pthread_mutex_destroy(&c->lock);
        pthread_cond_destroy(&c->ready_cv);
        pthread_cond_destroy(&c->send_cv);
        pthread_cond_destroy(&c->free_cv);
    }
    free(c);
}
