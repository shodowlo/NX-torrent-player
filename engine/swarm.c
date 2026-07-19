#include "swarm.h"

#include <stdlib.h>
#include <string.h>

#include <switch.h>
#include <mbedtls/sha1.h>

#include "peer.h"

// Per-piece state in the shared work queue.
enum { PIECE_NEEDED = 0, PIECE_INFLIGHT = 1, PIECE_DONE = 2 };

typedef struct {
    const torrent_meta *t;
    const uint8_t *peer_id;

    // Shared state, guarded by `lock`.
    Mutex lock;
    uint8_t *status;        // one byte per piece
    int64_t pieces_wanted;
    int64_t done_count;
    int active_workers;
    bool stop;

    // Peer pool: workers pull the next peer when they need one, so a dead peer
    // is replaced instead of killing the worker. The pool is cyclic (capped by
    // max_attempts) so the last few pieces get retried against good peers.
    peer_addr *peers;
    int peer_count;
    int next_peer;
    int max_attempts;

    // The output file has its own lock so hashing/writing doesn't serialize
    // on the same mutex as piece selection.
    Mutex file_lock;
    FILE *out;
} swarm_state;

typedef struct {
    swarm_state *s;
} worker_ctx;

// Claims the lowest-index needed piece this peer can serve, or -1 if none.
// Near-sequential claiming keeps the output roughly ordered, which helps the
// streaming mode we will add later.
static int64_t claim_piece(swarm_state *s, const peer_conn *p) {
    int64_t idx = -1;
    mutexLock(&s->lock);
    for (int64_t i = 0; i < s->pieces_wanted; i++) {
        if (s->status[i] == PIECE_NEEDED && peer_has_piece(p, i)) {
            s->status[i] = PIECE_INFLIGHT;
            idx = i;
            break;
        }
    }
    mutexUnlock(&s->lock);
    return idx;
}

static void mark(swarm_state *s, int64_t idx, uint8_t state) {
    mutexLock(&s->lock);
    s->status[idx] = state;
    mutexUnlock(&s->lock);
}

// Grabs the next peer from the (cyclic) pool, or returns false once the total
// attempt budget is spent.
static bool take_peer(swarm_state *s, peer_addr *out) {
    mutexLock(&s->lock);
    bool ok = s->next_peer < s->max_attempts;
    if (ok) *out = s->peers[s->next_peer++ % s->peer_count];
    mutexUnlock(&s->lock);
    return ok;
}

static bool all_done(swarm_state *s) {
    mutexLock(&s->lock);
    bool done = s->done_count >= s->pieces_wanted;
    mutexUnlock(&s->lock);
    return done;
}

// Downloads from one peer until it runs dry or fails. Returns nothing; the
// worker loop will fetch another peer afterwards.
static void drain_peer(swarm_state *s, peer_conn *p, uint8_t *buf) {
    char err[256];
    while (!s->stop) {
        int64_t idx = claim_piece(s, p);
        if (idx < 0) break;  // this peer has no piece we still need

        int64_t plen = torrent_piece_len(s->t, idx);
        if (peer_fetch_piece(p, idx, plen, buf, err, sizeof(err)) != 0) {
            mark(s, idx, PIECE_NEEDED);  // another worker will retry it
            break;
        }

        uint8_t hash[20];
        mbedtls_sha1(buf, plen, hash);
        if (memcmp(hash, s->t->piece_hashes + idx * 20, 20) != 0) {
            mark(s, idx, PIECE_NEEDED);
            break;
        }

        mutexLock(&s->file_lock);
        fseek(s->out, idx * s->t->piece_len, SEEK_SET);
        fwrite(buf, 1, plen, s->out);
        mutexUnlock(&s->file_lock);

        mutexLock(&s->lock);
        s->status[idx] = PIECE_DONE;
        s->done_count++;
        mutexUnlock(&s->lock);
    }
}

static void worker_main(void *arg) {
    worker_ctx *c = arg;
    swarm_state *s = c->s;
    char err[256];

    uint8_t *buf = malloc(s->t->piece_len);
    if (!buf) goto done;

    // Keep grabbing peers from the pool until pieces are done or peers run out.
    peer_addr addr;
    while (!s->stop && !all_done(s) && take_peer(s, &addr)) {
        peer_conn p;
        if (peer_connect(&p, addr, s->t->info_hash, s->peer_id,
                         s->t->piece_count, err, sizeof(err)) != 0)
            continue;
        if (peer_wait_unchoke(&p, err, sizeof(err)) != 0) {
            peer_disconnect(&p);
            continue;
        }
        drain_peer(s, &p, buf);
        peer_disconnect(&p);
    }

    free(buf);

done:
    mutexLock(&s->lock);
    s->active_workers--;
    mutexUnlock(&s->lock);
}

int64_t swarm_download(const torrent_meta *t, peer_addr *peers, int peer_count,
                       const uint8_t peer_id[20], FILE *out,
                       int64_t pieces_wanted, int max_workers) {
    if (pieces_wanted > t->piece_count) pieces_wanted = t->piece_count;
    if (peer_count <= 0) return 0;

    // More workers than peers is pointless; fewer peers means fewer workers.
    int n_workers = peer_count < max_workers ? peer_count : max_workers;

    swarm_state s = {0};
    mutexInit(&s.lock);
    mutexInit(&s.file_lock);
    s.t = t;
    s.peer_id = peer_id;
    s.pieces_wanted = pieces_wanted;
    s.out = out;
    s.active_workers = n_workers;
    s.peers = peers;
    s.peer_count = peer_count;
    s.next_peer = 0;
    s.max_attempts = peer_count * 3;  // allow a few passes over the pool
    s.status = calloc(1, t->piece_count);
    if (!s.status) return 0;

    Thread *threads = calloc(n_workers, sizeof(Thread));
    worker_ctx *ctxs = calloc(n_workers, sizeof(worker_ctx));
    if (!threads || !ctxs) {
        free(s.status); free(threads); free(ctxs);
        return 0;
    }

    for (int i = 0; i < n_workers; i++) {
        ctxs[i].s = &s;
        // stack_mem NULL -> libnx allocates; 0x2C is the default app priority.
        if (threadCreate(&threads[i], worker_main, &ctxs[i], NULL,
                         0x20000, 0x2C, -2) != 0) {
            mutexLock(&s.lock); s.active_workers--; mutexUnlock(&s.lock);
            threads[i].handle = 0;  // mark as not started
            continue;
        }
        threadStart(&threads[i]);
    }

    // Progress loop runs on the calling (console-owning) thread.
    u64 freq = armGetSystemTickFreq();
    u64 start = armGetSystemTick();
    u64 last_tick = start;
    int64_t last_done = 0;
    for (;;) {
        mutexLock(&s.lock);
        int64_t done = s.done_count;
        int active = s.active_workers;
        mutexUnlock(&s.lock);

        // Instantaneous rate over the interval since the previous refresh.
        u64 now = armGetSystemTick();
        u64 dt = now - last_tick;
        u64 kbps = 0;
        if (dt > 0)
            kbps = (u64)(done - last_done) * t->piece_len * freq / dt / 1024;
        last_tick = now;
        last_done = done;

        printf("\r  %lld/%lld pieces | %d peers | %llu Ko/s     ",
               (long long)done, (long long)pieces_wanted, active,
               (unsigned long long)kbps);
        consoleUpdate(NULL);

        if (done >= pieces_wanted) { s.stop = true; break; }
        if (active == 0) break;
        svcSleepThread(500000000ULL);  // 500 ms
    }
    u64 elapsed = armGetSystemTick() - start;

    for (int i = 0; i < n_workers; i++) {
        if (threads[i].handle == 0) continue;
        threadWaitForExit(&threads[i]);
        threadClose(&threads[i]);
    }

    int64_t done = s.done_count;

    // Report throughput.
    int64_t bytes = done * t->piece_len;
    if (elapsed > 0 && freq > 0) {
        u64 kbps = (u64)bytes * freq / elapsed / 1024;
        printf("\n  %lld Ko en %llu ms  =>  %llu Ko/s\n",
               (long long)(bytes / 1024),
               (unsigned long long)(elapsed * 1000 / freq),
               (unsigned long long)kbps);
    }

    free(threads);
    free(ctxs);
    free(s.status);
    return done;
}
