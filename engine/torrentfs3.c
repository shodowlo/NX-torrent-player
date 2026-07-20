// torrentfs3.c — v3 of the streaming torrent engine (same public API as v1/v2).
//
// A deliberate simplification of v2. What v2 bought with its complexity
// (global block scheduler, adaptive pipelines, upload serving, incoming
// acceptors, stalled-piece racing) was throughput headroom the player rarely
// needs: the stream plays at a few MB/s and the platform costs (bsd IPC per
// packet, SD latency, three CPU cores shared with mpv) dominate long before
// the scheduler does. v3 keeps the parts that were hard-won on this platform
// and deletes the rest:
//
//   KEPT (load-bearing, see v1/v2 history):
//   - one poll() netloop (libnx caps concurrent *blocking* BSD calls at 16)
//   - RAM piece assembly, SHA-1 from RAM, ONE sequential SD write per piece
//   - the FAT32-chunked, append-only cache (sparse writes freeze the console)
//   - raw fds, sliced writes under cache_lock (SD GC stalls must not block
//     the player's reads)
//   - lock-free playhead/have_piece on the mpv demuxer thread (an mpv thread
//     holding an engine lock while starved froze the netloop with it)
//   - bounded streaming window + startup head/tail criticals (moov atom)
//   - per-peer reconnect backoff, 130 s idle patience (> the 120 s keep-alive
//     interval: reaping sooner re-dials choking peers forever)
//   - backlog-driven calm mode + fill-rate governor (wifi bursts on the OS
//     core are what freeze the console)
//
//   DROPPED:
//   - upload serving, incoming acceptors, the listen socket (leech-only;
//     removes 2 threads, the INQ, tit-for-tat state)
//   - the global block scheduler (a session claims ONE piece at a time and
//     pipelines blocks within it; a stalled claim is parked -- buffer and
//     progress kept -- and adopted by the next session that has the piece)
//   - adaptive pipeline depth (fixed 48 blocks ~= 768 KB in flight per peer)
//   - separate announce and DHT threads (one discovery thread runs both)
//
// Session framing/handshake lives in peer.c's peer_nb layer instead of being
// duplicated here.

#include "torrentfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <switch.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <mbedtls/sha1.h>

#include "torrent.h"
#include "peer.h"      // MSG_* ids, BLOCK_LEN, bf_has_piece, peer_nb
#include "dhtclient.h"

//-----------------------------------------------------------------------------
// Tuning
//-----------------------------------------------------------------------------

#define MAX_SESS         24     // live peers; bounded by the socket buffer pool
#define MAX_CONNECTING   12     // slots allowed to sit in a pending connect
#define DIAL_STOP_LIVE   16     // enough live sessions: stop dialing new peers
#define CONNECT_SECS     2      // outbound SYN patience
#define PREHS_SECS       10     // connected but no handshake yet
#define IDLE_SECS        130    // handshaked, nothing received (see header)
#define KEEPALIVE_SECS   60
#define STALL_SECS       6      // requested blocks but nothing came: drop peer

#define DEPTH            48     // fixed request pipeline, in blocks (~768 KB)

// RAM for in-flight piece buffers; bounds how many pieces are open at once.
#define RAM_BUDGET       (48LL << 20)
#define AQ_MAX           16
#define AQ_MIN           4

// Streaming window: wide enough to feed the sessions, narrow enough that they
// don't advance 50 pieces in parallel while the player starves on one.
#define STREAM_WINDOW    (32LL * 1024 * 1024)
#define STREAM_MIN_PIECES 8

// Startup criticals: mpv probes the container head and tail (moov atom) first.
#define CRIT_HEAD        2
#define CRIT_TAIL        3

// FAT32 caps a file at 4 GB: the cache is chunked.
#define CACHE_CHUNK      (1LL << 30)
#define CACHE_MAX_CHUNKS 64
// Piece writes release cache_lock between slices of this size, so a slow SD
// write never makes the player's reads wait out the whole piece.
#define WR_SLICE         (256 * 1024)

// RAM streaming mode: verified pieces are kept in a bounded RAM window instead
// of being written to the SD card. Completing a piece then never bursts the
// FAT filesystem service on the OS core -- the SD write is what stutters
// playback at each piece boundary, the more so the bigger the piece. Once the
// resident bytes pass RAM_STREAM_BUDGET the pieces furthest behind the playhead
// are dropped; a read that lands on a dropped piece re-downloads it (the read
// moves the playhead there, so the streaming window covers it). Sized to leave
// the forward window plenty of seek-back slack; needs a full-RAM launch.
#define RAM_STREAM_BUDGET (256LL << 20)

#define TFS_MAX_PEERS    128
#define BACKOFF_CONN_SECS 15
#define BACKOFF_MAX_SECS  300
#define BACKOFF_DROP_SECS 30

// Discovery re-runs every 15 min, or 60 s after the last round when starved.
#define DISC_INTERVAL_SECS (15 * 60)
#define DISC_STARVED_SECS  60
#define STARVED_LIVE       3

enum { PIECE_NEEDED = 0, PIECE_ACTIVE = 1, PIECE_DONE = 2, PIECE_WRITING = 3 };

static volatile int g_governor = 1;
void torrentfs_set_governor(int on) { g_governor = on ? 1 : 0; }
int  torrentfs_governor(void)       { return g_governor; }

// Read once per torrent at open time (like the peer id); a live torrent keeps
// whatever mode it opened with. Set it before torrentfs_open.
static volatile int g_ram_stream = 0;
void torrentfs_set_ram_stream(int on) { g_ram_stream = on ? 1 : 0; }
int  torrentfs_ram_stream(void)       { return g_ram_stream; }

//-----------------------------------------------------------------------------
// Types
//-----------------------------------------------------------------------------

// One piece being assembled in RAM. idx -1 = slot free. owner is the session
// currently requesting its blocks (-1 = parked: progress kept, waiting to be
// adopted). Only the netloop touches entries, except the writer returning a
// buffer under t->lock.
typedef struct {
    int64_t idx;
    int owner;          // session id, or -1 (parked)
    uint8_t *buf;       // piece_len bytes, lazily allocated, reused
    uint8_t *have;      // one byte per block: data present in buf
    uint8_t *req;       // one byte per block: requested by the owner (or have)
    int nblocks;
    int have_cnt;
    int next_req;       // scan cursor for the next block to request
} aq_entry;

typedef struct {
    bool active;
    bool connecting;
    peer_nb nb;         // sock/framing/handshake/bitfield (peer.c)
    int pidx;           // peer pool index (for backoff bookkeeping)
    int64_t claim;      // piece being fetched, -1 = none
    int out_n;          // block requests outstanding
    u64 started, last_rx, last_block, last_ka;
} sess;

typedef struct {
    int64_t idx;
    int64_t plen;
    uint8_t *buf;
} wjob;

struct torrentfs {
    torrent_meta meta;

    int64_t stream_offset, stream_size;
    int64_t file_first_piece, file_last_piece;
    int blocks_per_piece;      // of a full piece

    // FAT32-chunked cache (see v2 for the append-only rationale).
    int chunks[CACHE_MAX_CHUNKS];
    char cache_base[192];
    Mutex cache_lock;
    int32_t *piece_slot;       // piece index -> cache slot, -1 = not stored
    int64_t next_slot;
    int64_t slots_per_chunk;

    // RAM streaming window (ram_mode). ram_piece[idx] is the resident buffer for
    // a verified piece, or NULL. Both the writer (store/evict) and the reader
    // touch it under cache_lock. ram_lo is a scan cursor for the eviction sweep.
    bool ram_mode;
    uint8_t **ram_piece;
    int64_t ram_budget;
    int64_t ram_resident;
    int64_t ram_peak;
    int64_t ram_lo;

    // t->lock guards: status[] transitions shared with the writer, the writer
    // queue, buffer returns, the peer pool (shared with discovery), counters
    // the UI reads. Session/entry state is netloop-private and needs no lock.
    Mutex lock;
    uint8_t *status;
    int64_t pieces_done;
    int64_t playhead_piece;    // lock-free (aligned 64-bit store, see below)
    volatile bool stop;

    aq_entry aq[AQ_MAX];
    int n_aq;

    wjob wq[AQ_MAX + 2];
    int wq_head, wq_n;

    sess S[MAX_SESS];

    // Peer pool + backoff.
    peer_addr peers[TFS_MAX_PEERS];
    uint8_t peer_fails[TFS_MAX_PEERS];
    uint8_t peer_busy[TFS_MAX_PEERS];
    u64 peer_next_try[TFS_MAX_PEERS];
    int peer_count;
    int next_peer;

    uint8_t peer_id[20];

    Thread netloop, writer, discovery;
    bool netloop_started, writer_started, discovery_started;

    u64 freq;

    // Calm / governor.
    int backlog_ms;            // written by the app thread, read racily
    int calm_now;
    double rate_bps;           // netloop-only EWMA
    int64_t rate_last_bytes;
    u64 rate_last_tick;

    // Debug surface (racy reads by the UI are fine; single-writer counters).
    int st_conn_ok, st_conn_fail, st_sock_fail, st_conn_timeout;
    int st_unchoke_ok, st_choked;
    int st_piece_ok, st_fetch_fail, st_sha_fail;
    int st_interested_recv, st_request_recv;
    int st_bf_empty, st_bf_ok, st_bf_bad;
    int st_live, st_peak_live, st_connecting;
    int st_claim_ok, st_claim_fail;
    int st_cache_wr_fail, st_cache_rd_short;
    int64_t st_cache_written;
    int64_t st_bytes_recv;
    int64_t st_blocks_have;    // blocks resident in RAM partials
    int64_t st_win_ph, st_win_lo, st_win_hi;
    char st_last_err[128];

    u64 hb_tick[4];
    u8  hb_core[4];
    uint32_t lat_n[5];
    uint64_t lat_max[5];
};

enum { LAT_POLL = 0, LAT_RECV, LAT_SEND, LAT_WR, LAT_RD };
enum { HB_NET = 0, HB_WRITER = 1, HB_READER = 2, HB_UI = 3 };

static void hb_beat(torrentfs *t, int k) {
    t->hb_tick[k] = armGetSystemTick();
    t->hb_core[k] = (u8)svcGetCurrentProcessorNumber();
}

static void lat_add(torrentfs *t, int c, u64 t0) {
    u64 d = armGetSystemTick() - t0;
    t->lat_n[c]++;
    if (d > t->lat_max[c]) t->lat_max[c] = d;
}

static uint32_t block_len_of(int64_t plen, int b) {
    int64_t rem = plen - (int64_t)b * BLOCK_LEN;
    return rem > BLOCK_LEN ? BLOCK_LEN : (uint32_t)rem;
}

static void set_err(torrentfs *t, const char *fmt, long long a) {
    snprintf(t->st_last_err, sizeof(t->st_last_err), fmt, a);
}

//-----------------------------------------------------------------------------
// Chunked cache I/O (ported from v2 -- append-only on purpose, FAT has no
// sparse files: a write past EOF journals every cluster in between on the OS
// core while we hold cache_lock. Raw fds, not stdio: newlib's FILE* buffer
// turns one piece into thousands of fs IPCs.)
//-----------------------------------------------------------------------------

static int cache_chunk(torrentfs *t, int ci) {
    if (ci < 0 || ci >= CACHE_MAX_CHUNKS) return -1;
    if (t->chunks[ci] >= 0) return t->chunks[ci];
    char path[256];
    snprintf(path, sizeof(path), "%s.%03d", t->cache_base, ci);
    int f = open(path, O_RDWR | O_CREAT, 0666);
    t->chunks[ci] = f;
    return f;
}

static bool fd_xfer(int f, int64_t off, uint8_t *p, size_t len, bool wr) {
    if (lseek(f, (off_t)off, SEEK_SET) < 0) return false;
    while (len > 0) {
        ssize_t d = wr ? write(f, p, len) : read(f, p, len);
        if (d <= 0) return false;
        p += d;
        len -= (size_t)d;
    }
    return true;
}

static bool piece_loc(torrentfs *t, int64_t idx, bool alloc, int *ci,
                      int64_t *co) {
    int32_t slot = t->piece_slot[idx];
    if (slot < 0) {
        if (!alloc) return false;
        if (t->next_slot >= (int64_t)CACHE_MAX_CHUNKS * t->slots_per_chunk)
            return false;
        slot = (int32_t)t->next_slot++;
        t->piece_slot[idx] = slot;
    }
    *ci = (int)(slot / t->slots_per_chunk);
    *co = (int64_t)(slot % t->slots_per_chunk) * t->meta.piece_len;
    return true;
}

static bool cache_piece_read(torrentfs *t, int64_t idx, int64_t within,
                             void *buf, size_t len) {
    if (t->ram_mode) {
        // Caller (cache_read_upto) holds cache_lock, so the buffer cannot be
        // freed by an eviction mid-copy. A dropped piece reads short, which the
        // player path tolerates (and the read moves the playhead here, pulling
        // the piece back into the window).
        uint8_t *src = t->ram_piece[idx];
        if (!src) return false;
        memcpy(buf, src + within, len);
        return true;
    }
    int ci;
    int64_t co;
    if (!piece_loc(t, idx, false, &ci, &co)) return false;
    int f = cache_chunk(t, ci);
    if (f < 0) return false;
    return fd_xfer(f, co + within, buf, len, false);
}

static void cache_delete_all(torrentfs *t) {
    for (int i = 0; i < CACHE_MAX_CHUNKS; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s.%03d", t->cache_base, i);
        remove(path);
    }
}

// Read as much as is there; the player path must never hard-fail (mpv treats
// an error as a dead stream and stops reading for good).
static size_t cache_read_upto(torrentfs *t, int64_t off, void *buf, size_t len) {
    uint8_t *p = buf;
    size_t total = 0;
    int64_t plen = t->meta.piece_len;
    while (len > 0) {
        int64_t idx    = off / plen;
        int64_t within = off % plen;
        size_t n       = len;
        if (within + (int64_t)n > plen) n = (size_t)(plen - within);
        if (!cache_piece_read(t, idx, within, p, n)) break;
        total += n;
        p += n; off += (int64_t)n; len -= n;
    }
    return total;
}

//-----------------------------------------------------------------------------
// RAM streaming window (ram_mode). Verified pieces stay in RAM rather than
// going to the SD card, so a piece completing never bursts the FAT filesystem
// service on the OS core. Bounded: over budget, the pieces furthest behind the
// playhead are dropped (status DONE -> NEEDED, so a later read that lands on one
// re-downloads it -- the read moves the playhead there, and the streaming
// window only fetches forward, never backfilling what was intentionally
// dropped). All state is touched under cache_lock.
//-----------------------------------------------------------------------------

// Lock order note: this takes t->lock while holding cache_lock. No path takes
// them the other way round (the writer's SD path releases cache_lock before it
// touches t->lock), so the nesting cannot deadlock.
static void ram_evict(torrentfs *t) {   // caller holds t->cache_lock
    int64_t ph = t->playhead_piece;
    while (t->ram_resident > t->ram_budget) {
        while (t->ram_lo < ph && !t->ram_piece[t->ram_lo]) t->ram_lo++;
        if (t->ram_lo >= ph) break;     // nothing strictly behind left to drop
        int64_t v = t->ram_lo;
        free(t->ram_piece[v]);
        t->ram_piece[v] = NULL;
        t->ram_resident -= t->meta.piece_len;
        mutexLock(&t->lock);
        if (t->status[v] == PIECE_DONE) { t->status[v] = PIECE_NEEDED; t->pieces_done--; }
        mutexUnlock(&t->lock);
    }
}

// Move a verified piece buffer into the window, taking ownership of buf.
static void ram_store(torrentfs *t, int64_t idx, uint8_t *buf) {
    mutexLock(&t->cache_lock);
    if (t->ram_piece[idx]) {            // a re-download of a still-resident piece
        free(t->ram_piece[idx]);
        t->ram_resident -= t->meta.piece_len;
    }
    t->ram_piece[idx] = buf;
    t->ram_resident += t->meta.piece_len;
    if (idx < t->ram_lo) t->ram_lo = idx;   // seek-back reload below the cursor
    if (t->ram_resident > t->ram_peak) t->ram_peak = t->ram_resident;
    ram_evict(t);
    mutexUnlock(&t->cache_lock);
}

//-----------------------------------------------------------------------------
// Assembly entries
//-----------------------------------------------------------------------------

static aq_entry *aq_find(torrentfs *t, int64_t idx) {
    for (int i = 0; i < t->n_aq; i++)
        if (t->aq[i].idx == idx) return &t->aq[i];
    return NULL;
}

// A free slot WITH a buffer (buffers move to the writer and come back; a slot
// whose buffer is out cannot start a new piece). The scan-and-take runs under
// t->lock: the writer picks a return slot by the same idx/buf fields, and a
// half-taken slot seen from the other thread would double-assign a buffer.
static aq_entry *aq_alloc(torrentfs *t, int64_t idx) {
    aq_entry *a = NULL;
    mutexLock(&t->lock);
    for (int i = 0; i < t->n_aq; i++) {
        aq_entry *c = &t->aq[i];
        if (c->idx >= 0) continue;
        if (!c->buf) c->buf = malloc((size_t)t->meta.piece_len);
        if (!c->buf) continue;
        c->idx = idx;
        a = c;
        break;
    }
    mutexUnlock(&t->lock);
    if (!a) return NULL;
    a->owner    = -1;
    a->have_cnt = 0;
    a->next_req = 0;
    a->nblocks  =
        (int)((torrent_piece_len(&t->meta, idx) + BLOCK_LEN - 1) / BLOCK_LEN);
    memset(a->have, 0, (size_t)t->blocks_per_piece);
    memset(a->req, 0, (size_t)t->blocks_per_piece);
    return a;
}

// Park: keep buffer and progress, forget who was fetching it. Blocks the old
// owner still had on order will either arrive (stored anyway -- lookups are by
// piece, not by owner) or never come; the adopter re-requests what's missing.
static void aq_park(torrentfs *t, aq_entry *a) {
    a->owner    = -1;
    a->next_req = 0;
    memcpy(a->req, a->have, (size_t)a->nblocks);
}

//-----------------------------------------------------------------------------
// Peer pool (shared with the discovery thread -> under t->lock)
//-----------------------------------------------------------------------------

static void add_peers_cb(void *ctx, const peer_addr *peers, int n) {
    torrentfs *t = ctx;
    mutexLock(&t->lock);
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int j = 0; j < t->peer_count; j++)
            if (t->peers[j].ip == peers[i].ip &&
                t->peers[j].port == peers[i].port) { dup = true; break; }
        if (dup) continue;
        int slot = -1;
        if (t->peer_count < TFS_MAX_PEERS) {
            slot = t->peer_count++;
        } else {
            for (int j = 0; j < TFS_MAX_PEERS; j++) {
                if (t->peer_busy[j] || t->peer_fails[j] == 0) continue;
                if (slot < 0 || t->peer_fails[j] > t->peer_fails[slot]) slot = j;
            }
        }
        if (slot < 0) continue;
        t->peers[slot]         = peers[i];
        t->peer_fails[slot]    = 0;
        t->peer_next_try[slot] = 0;
    }
    mutexUnlock(&t->lock);
}

static int take_peer(torrentfs *t, peer_addr *out) {
    u64 now = armGetSystemTick();
    mutexLock(&t->lock);
    int idx = -1;
    if (!t->stop && t->peer_count > 0) {
        for (int tries = 0; tries < t->peer_count; tries++) {
            int i = t->next_peer++ % t->peer_count;
            if (t->peer_busy[i]) continue;
            if (t->peer_next_try[i] > now) continue;
            idx = i;
            *out = t->peers[i];
            t->peer_busy[i] = 1;
            break;
        }
    }
    mutexUnlock(&t->lock);
    return idx;
}

static void release_peer(torrentfs *t, int pidx, bool failed, bool had_conn) {
    if (pidx < 0) return;
    mutexLock(&t->lock);
    t->peer_busy[pidx] = 0;
    if (failed) {
        int f = ++t->peer_fails[pidx];
        int secs = had_conn ? BACKOFF_DROP_SECS : BACKOFF_CONN_SECS * (1 << (f > 5 ? 5 : f - 1));
        if (secs > BACKOFF_MAX_SECS) secs = BACKOFF_MAX_SECS;
        t->peer_next_try[pidx] = armGetSystemTick() + (u64)secs * t->freq;
    } else {
        t->peer_fails[pidx]    = 0;
        t->peer_next_try[pidx] = armGetSystemTick() + (u64)BACKOFF_DROP_SECS * t->freq;
    }
    mutexUnlock(&t->lock);
}

//-----------------------------------------------------------------------------
// Discovery (one thread: trackers, then DHT, then wait)
//-----------------------------------------------------------------------------

static bool file_done(torrentfs *t) {
    mutexLock(&t->lock);
    bool d = t->pieces_done >= t->file_last_piece - t->file_first_piece + 1;
    mutexUnlock(&t->lock);
    return d;
}

static bool discovery_wait(torrentfs *t) {
    u64 start = armGetSystemTick();
    while (!t->stop && !file_done(t)) {
        svcSleepThread(1000000000ULL);  // 1 s
        u64 secs = (armGetSystemTick() - start) / t->freq;
        if (secs >= DISC_INTERVAL_SECS) return true;
        if (secs >= DISC_STARVED_SECS && t->st_live < STARVED_LIVE) return true;
    }
    return false;
}

static void discovery_main(void *arg) {
    torrentfs *t = arg;
    char e[128];
    do {
        torrent_announce_cb(&t->meta, add_peers_cb, t, e, sizeof(e));
        if (t->stop) break;
        dht_find_peers(t->meta.info_hash, 100, 15000, add_peers_cb, t,
                       &t->stop, e, sizeof(e));
    } while (discovery_wait(t));
}

//-----------------------------------------------------------------------------
// Writer: verify from RAM, one sequential (sliced) SD write, mark DONE.
//-----------------------------------------------------------------------------

static void writer_main(void *arg) {
    torrentfs *t = arg;
    for (;;) {
        hb_beat(t, HB_WRITER);
        wjob j;
        bool has = false;
        mutexLock(&t->lock);
        if (t->wq_n > 0) {
            j = t->wq[t->wq_head];
            t->wq_head = (t->wq_head + 1) % (AQ_MAX + 2);
            t->wq_n--;
            has = true;
        }
        bool stopping = t->stop;
        mutexUnlock(&t->lock);

        if (!has) {
            if (stopping) break;   // drained: nothing more can be queued
            svcSleepThread(2000000ULL);  // 2 ms
            continue;
        }

        int nb = (int)((j.plen + BLOCK_LEN - 1) / BLOCK_LEN);
        uint8_t hash[20];
        mbedtls_sha1(j.buf, (size_t)j.plen, hash);
        bool ok = memcmp(hash, t->meta.piece_hashes + j.idx * 20, 20) == 0;

        if (ok && t->ram_mode) {
            // No SD write at all: hand the buffer to the RAM window and mark the
            // piece done. This is the whole point of the mode -- the per-piece
            // FAT write that stutters playback simply never happens.
            ram_store(t, j.idx, j.buf);
            j.buf = NULL;                          // ownership moved to the window
            mutexLock(&t->lock);
            if (t->status[j.idx] != PIECE_DONE) {
                t->status[j.idx] = PIECE_DONE;
                t->pieces_done++;
            }
            t->st_piece_ok++;
            t->st_cache_written += j.plen;
        } else if (ok) {
            mutexLock(&t->cache_lock);
            int wci = -1;
            int64_t wco = 0;
            ok = piece_loc(t, j.idx, true, &wci, &wco);
            mutexUnlock(&t->cache_lock);
            u64 lt0 = armGetSystemTick();
            size_t woff = 0;
            while (ok && woff < (size_t)j.plen) {
                size_t n = (size_t)j.plen - woff;
                if (n > WR_SLICE) n = WR_SLICE;
                mutexLock(&t->cache_lock);
                int f = cache_chunk(t, wci);
                ok = f >= 0 && fd_xfer(f, wco + (int64_t)woff, j.buf + woff, n, true);
                mutexUnlock(&t->cache_lock);
                woff += n;
            }
            lat_add(t, LAT_WR, lt0);

            mutexLock(&t->lock);
            if (ok) {
                if (t->status[j.idx] != PIECE_DONE) {
                    t->status[j.idx] = PIECE_DONE;
                    t->pieces_done++;
                }
                t->st_piece_ok++;
                t->st_cache_written += j.plen;
            } else {
                t->status[j.idx] = PIECE_NEEDED;   // lost: re-download
                t->st_cache_wr_fail++;
                set_err(t, "cache write failed, piece %lld", (long long)j.idx);
            }
        } else {
            mutexLock(&t->lock);
            t->status[j.idx] = PIECE_NEEDED;       // corrupt: re-download
            t->st_sha_fail++;
            set_err(t, "sha fail piece %lld", (long long)j.idx);
        }
        t->st_blocks_have -= nb;
        // Return the buffer to a bufferless slot for reuse.
        for (int i = 0; i < t->n_aq; i++)
            if (t->aq[i].idx < 0 && !t->aq[i].buf) { t->aq[i].buf = j.buf; j.buf = NULL; break; }
        mutexUnlock(&t->lock);
        free(j.buf);   // no slot took it (shouldn't happen); don't leak
    }
}

//-----------------------------------------------------------------------------
// Sessions
//-----------------------------------------------------------------------------

static void sess_close(torrentfs *t, sess *s, bool failed) {
    if (!s->active) return;
    if (s->claim >= 0) {
        aq_entry *a = aq_find(t, s->claim);
        if (a) aq_park(t, a);
        s->claim = -1;
    }
    bool had_conn = s->nb.handshaked;
    if (s->connecting) {
        // still a raw socket; peer_nb was never attached
        if (s->nb.sock >= 0) close(s->nb.sock);
        s->nb.sock = -1;
        t->st_connecting--;
    } else {
        if (s->nb.handshaked) t->st_live--;
        peer_nb_free(&s->nb);
    }
    release_peer(t, s->pidx, failed, had_conn);
    s->active = false;
    s->connecting = false;
    s->out_n = 0;
}

// Start a non-blocking dial. Returns false when no session/peer is available.
static bool sess_dial(torrentfs *t, u64 now) {
    int sid = -1;
    for (int i = 0; i < MAX_SESS; i++)
        if (!t->S[i].active) { sid = i; break; }
    if (sid < 0) return false;

    peer_addr pa;
    int pidx = take_peer(t, &pa);
    if (pidx < 0) return false;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        t->st_sock_fail++;
        t->st_conn_fail++;
        set_err(t, "socket(): errno %lld", (long long)errno);
        release_peer(t, pidx, true, false);
        return false;
    }
    int fl = fcntl(sock, F_GETFL, 0);
    if (fl >= 0) fcntl(sock, F_SETFL, fl | O_NONBLOCK);

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = pa.ip;
    sa.sin_port = htons(pa.port);
    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0 &&
        errno != EINPROGRESS) {
        t->st_sock_fail++;
        t->st_conn_fail++;
        close(sock);
        release_peer(t, pidx, true, false);
        return true;   // slot still free; try another peer next tick
    }

    sess *s = &t->S[sid];
    memset(s, 0, sizeof(*s));
    s->active     = true;
    s->connecting = true;
    s->nb.sock    = sock;   // raw until the connect completes
    s->pidx       = pidx;
    s->claim      = -1;
    s->started    = now;
    t->st_connecting++;
    return true;
}

// Pending connect finished (POLLOUT): attach the peer_nb layer and handshake.
static void sess_connected(torrentfs *t, sess *s, u64 now) {
    int soerr = 0;
    socklen_t sl = sizeof(soerr);
    getsockopt(s->nb.sock, SOL_SOCKET, SO_ERROR, &soerr, &sl);
    if (soerr != 0) {
        t->st_conn_fail++;
        t->st_conn_timeout++;   // most refusals here are dead/NAT'd peers
        sess_close(t, s, true);
        return;
    }
    int sock = s->nb.sock;
    if (peer_nb_init(&s->nb, sock, t->meta.piece_count) != 0) {
        close(sock);
        s->nb.sock = -1;
        t->st_sock_fail++;
        sess_close(t, s, true);
        return;
    }
    s->connecting = false;
    t->st_connecting--;
    t->st_conn_ok++;
    s->last_rx = s->last_ka = now;
    peer_nb_send_handshake(&s->nb, t->meta.info_hash, t->peer_id);
    peer_nb_flush(&s->nb);
}

//-----------------------------------------------------------------------------
// Claiming and the request pipeline
//-----------------------------------------------------------------------------

// Streaming window; also published for the debug panel.
static void calc_window(torrentfs *t, int64_t *ph, int64_t *lo, int64_t *hi) {
    int64_t p = t->playhead_piece;   // lock-free read (netloop only writes stats)
    int64_t flo = t->file_first_piece, fhi = t->file_last_piece;
    if (p < flo) p = flo;
    if (p > fhi) p = fhi;
    int64_t win = STREAM_WINDOW / t->meta.piece_len;
    if (win < STREAM_MIN_PIECES) win = STREAM_MIN_PIECES;
    int64_t h = p + win;
    if (h > fhi + 1) h = fhi + 1;
    *ph = p; *lo = flo; *hi = h;
    t->st_win_ph = p; t->st_win_lo = flo; t->st_win_hi = h;
}

// Can this session take piece idx? NEEDED needs a free buffer; a parked
// ACTIVE entry is adopted with its progress.
static bool try_claim(torrentfs *t, sess *s, int sid, int64_t idx) {
    if (idx < t->file_first_piece || idx > t->file_last_piece) return false;
    uint8_t st = t->status[idx];
    if (st == PIECE_DONE || st == PIECE_WRITING) return false;
    if (!bf_has_piece(s->nb.bitfield, s->nb.bitfield_len, idx)) return false;

    aq_entry *a;
    if (st == PIECE_ACTIVE) {
        a = aq_find(t, idx);
        if (!a || a->owner >= 0) return false;   // someone else is on it
    } else {
        a = aq_alloc(t, idx);
        if (!a) return false;                    // no buffer free right now
        mutexLock(&t->lock);
        t->status[idx] = PIECE_ACTIVE;
        mutexUnlock(&t->lock);
    }
    a->owner = sid;
    s->claim = idx;
    s->out_n = 0;
    s->last_block = armGetSystemTick();
    return true;
}

// Pick a piece for an idle unchoked session: startup criticals, then the
// playhead window in order, then backfill behind the playhead.
static void claim_piece(torrentfs *t, sess *s, int sid) {
    int64_t ph, lo, hi;
    calc_window(t, &ph, &lo, &hi);
    int64_t fhi = t->file_last_piece;

    if (ph <= lo + CRIT_HEAD) {   // startup: tail (moov) then head
        for (int64_t i = fhi; i > fhi - CRIT_TAIL && i >= lo; i--)
            if (try_claim(t, s, sid, i)) { t->st_claim_ok++; return; }
        for (int64_t i = lo; i < lo + CRIT_HEAD && i <= fhi; i++)
            if (try_claim(t, s, sid, i)) { t->st_claim_ok++; return; }
    }
    for (int64_t i = ph; i < hi; i++)
        if (try_claim(t, s, sid, i)) { t->st_claim_ok++; return; }
    // Backfill behind the playhead completes the whole file for the SD cache.
    // In RAM mode there is no cache to complete and pieces behind the playhead
    // are dropped on purpose -- refetching them would just churn the window --
    // so stream strictly forward. A seek back re-enters the range above via the
    // window (a read moves the playhead), which does refetch what it needs.
    if (!t->ram_mode)
        for (int64_t i = lo; i < ph; i++)
            if (try_claim(t, s, sid, i)) { t->st_claim_ok++; return; }
    t->st_claim_fail++;
}

// Keep the pipeline full for the session's claimed piece.
static void fill_pipeline(torrentfs *t, sess *s) {
    if (s->claim < 0 || s->nb.choked) return;
    aq_entry *a = aq_find(t, s->claim);
    if (!a) { s->claim = -1; return; }
    int64_t plen = torrent_piece_len(&t->meta, s->claim);
    while (s->out_n < DEPTH && a->next_req < a->nblocks) {
        int b = a->next_req;
        if (a->req[b]) { a->next_req++; continue; }
        uint8_t pl[12];
        uint32_t v;
        v = htonl((uint32_t)s->claim);        memcpy(pl, &v, 4);
        v = htonl((uint32_t)b * BLOCK_LEN);   memcpy(pl + 4, &v, 4);
        v = htonl(block_len_of(plen, b));     memcpy(pl + 8, &v, 4);
        if (peer_nb_queue(&s->nb, MSG_REQUEST, pl, 12) != 0) break;
        a->req[b] = 1;
        a->next_req++;
        s->out_n++;
    }
}

// All blocks landed: hand the buffer to the writer (which verifies + writes).
static void piece_full(torrentfs *t, aq_entry *a) {
    int owner = a->owner;
    mutexLock(&t->lock);
    t->status[a->idx] = PIECE_WRITING;
    int tail = (t->wq_head + t->wq_n) % (AQ_MAX + 2);
    t->wq[tail] = (wjob){ a->idx, torrent_piece_len(&t->meta, a->idx), a->buf };
    t->wq_n++;                 // can't overflow: each job holds one aq buffer
    a->idx = -1;               // slot reset inside the lock: the writer picks
    a->buf = NULL;             // its return slot by these very fields
    a->owner = -1;
    mutexUnlock(&t->lock);
    if (owner >= 0 && owner < MAX_SESS && t->S[owner].claim >= 0) {
        t->S[owner].claim = -1;
        t->S[owner].out_n = 0;
    }
}

//-----------------------------------------------------------------------------
// Message handling
//-----------------------------------------------------------------------------

static void sess_msg(torrentfs *t, sess *s, int sid, uint8_t id, uint8_t *pl,
                     uint32_t plen, u64 now) {
    (void)sid;
    switch (id) {
        case MSG_CHOKE:
            s->nb.choked = true;
            t->st_choked++;
            if (s->claim >= 0) {   // outstanding requests are void now
                aq_entry *a = aq_find(t, s->claim);
                if (a) aq_park(t, a);
                s->claim = -1;
                s->out_n = 0;
            }
            break;
        case MSG_UNCHOKE:
            s->nb.choked = false;
            t->st_unchoke_ok++;
            break;
        case MSG_INTERESTED:
            t->st_interested_recv++;
            break;
        case MSG_REQUEST:
            t->st_request_recv++;   // leech-only: we never unchoke, ignore
            break;
        case MSG_HAVE:
            if (plen == 4) {
                uint32_t v;
                memcpy(&v, pl, 4);
                int64_t idx = (int64_t)ntohl(v);
                if (idx >= 0 && idx < t->meta.piece_count)
                    s->nb.bitfield[idx / 8] |= (uint8_t)(0x80 >> (idx % 8));
            }
            break;
        case MSG_BITFIELD:
            if (plen == s->nb.bitfield_len) {
                memcpy(s->nb.bitfield, pl, plen);
                t->st_bf_ok++;
            } else {
                t->st_bf_bad++;
            }
            break;
        case MSG_PIECE: {
            if (plen < 8) break;
            uint32_t iv, bv;
            memcpy(&iv, pl, 4);
            memcpy(&bv, pl + 4, 4);
            int64_t idx     = (int64_t)ntohl(iv);
            uint32_t begin  = ntohl(bv);
            uint32_t dlen   = plen - 8;
            if (idx < 0 || idx >= t->meta.piece_count) break;
            if (begin % BLOCK_LEN != 0) break;
            int b = (int)(begin / BLOCK_LEN);
            int64_t p_len = torrent_piece_len(&t->meta, idx);
            if (dlen != block_len_of(p_len, b)) break;

            t->st_bytes_recv += dlen;
            s->last_block = now;
            if (s->out_n > 0) s->out_n--;

            // Stored by piece, not by owner: a parked piece's stragglers (or
            // an adopted piece's duplicates) still count.
            aq_entry *a = aq_find(t, idx);
            if (!a || b >= a->nblocks || a->have[b]) break;
            memcpy(a->buf + begin, pl + 8, dlen);
            a->have[b] = 1;
            a->req[b]  = 1;
            a->have_cnt++;
            mutexLock(&t->lock);
            t->st_blocks_have++;
            mutexUnlock(&t->lock);
            if (a->have_cnt == a->nblocks) piece_full(t, a);
            break;
        }
        default:
            break;   // MSG_CANCEL / MSG_NOT_INTERESTED: nothing useful to do
    }
}

static void sess_service(torrentfs *t, sess *s, int sid, u64 now) {
    ssize_t got = peer_nb_pump_rx(&s->nb);
    if (got < 0) { t->st_fetch_fail++; sess_close(t, s, true); return; }
    if (got > 0) s->last_rx = now;

    if (!s->nb.handshaked) {
        int hs = peer_nb_recv_handshake(&s->nb, t->meta.info_hash);
        if (hs < 0) { sess_close(t, s, true); return; }
        if (hs == 0) return;
        t->st_live++;
        if (t->st_live > t->st_peak_live) t->st_peak_live = t->st_live;
        peer_nb_queue(&s->nb, MSG_INTERESTED, NULL, 0);
    }

    for (;;) {
        uint8_t id;
        uint8_t *pl;
        uint32_t plen;
        int r = peer_nb_next(&s->nb, &id, &pl, &plen);
        if (r < 0) { sess_close(t, s, true); return; }
        if (r == 0) break;
        sess_msg(t, s, sid, id, pl, plen, now);
        if (!s->active) return;
    }

    fill_pipeline(t, s);
    if (peer_nb_flush(&s->nb) != 0) { sess_close(t, s, true); return; }
}

//-----------------------------------------------------------------------------
// Netloop
//-----------------------------------------------------------------------------

// Calm budget: how many sessions may hold a claim, from the player's backlog.
// The whole swarm bursting at wifi line rate on every window slide is what
// freezes the console (bsd/wlan pay per packet on the OS core).
static int calm_budget(torrentfs *t) {
    int ms = t->backlog_ms;   // racy read, written by the app thread
    int budget = ms >= 30000 ? 1 : ms >= 20000 ? 3 : ms >= 10000 ? 8 : MAX_SESS;

    if (g_governor && ms >= 10000) {
        // Rate above the backlog-tied target: pause claiming entirely.
        double target = ms >= 25000
                            ? 1.5e6
                            : 6.0e6 - (ms - 10000) * (4.5e6 / 15000.0);
        if (t->rate_bps > target) budget = 0;
    }
    return budget;
}

static void netloop_main(void *arg) {
    torrentfs *t = arg;
    u64 last_upkeep = 0;

    while (!t->stop) {
        hb_beat(t, HB_NET);
        u64 now = armGetSystemTick();

        // Upkeep every ~500 ms: dial, reap, keep-alives, rate EWMA, calm.
        if (now - last_upkeep > t->freq / 2) {
            last_upkeep = now;

            // Download-rate EWMA (governor input).
            if (t->rate_last_tick) {
                double dt = (double)(now - t->rate_last_tick) / t->freq;
                if (dt > 0.05) {
                    double inst = (double)(t->st_bytes_recv - t->rate_last_bytes) / dt;
                    t->rate_bps = t->rate_bps * 0.7 + inst * 0.3;
                }
            }
            t->rate_last_bytes = t->st_bytes_recv;
            t->rate_last_tick  = now;
            t->calm_now        = calm_budget(t);

            for (int i = 0; i < MAX_SESS; i++) {
                sess *s = &t->S[i];
                if (!s->active) continue;
                u64 age = now - (s->connecting ? s->started : s->last_rx);
                if (s->connecting && age > (u64)CONNECT_SECS * t->freq) {
                    t->st_conn_fail++;
                    t->st_conn_timeout++;
                    sess_close(t, s, true);
                    continue;
                }
                if (!s->connecting && !s->nb.handshaked &&
                    now - s->started > (u64)PREHS_SECS * t->freq) {
                    sess_close(t, s, true);
                    continue;
                }
                if (!s->connecting && age > (u64)IDLE_SECS * t->freq) {
                    sess_close(t, s, true);
                    continue;
                }
                // Requested blocks and nothing came: the peer accepted work it
                // will not deliver -- drop it, its claim gets adopted.
                if (s->out_n > 0 &&
                    now - s->last_block > (u64)STALL_SECS * t->freq) {
                    t->st_fetch_fail++;
                    set_err(t, "stall, piece %lld", (long long)s->claim);
                    sess_close(t, s, true);
                    continue;
                }
                // Keep-alive: raw 4 zero bytes, only when the tx queue is
                // empty (an atomic small send cannot interleave mid-message).
                if (!s->connecting && s->nb.handshaked &&
                    now - s->last_ka > (u64)KEEPALIVE_SECS * t->freq &&
                    !peer_nb_tx_pending(&s->nb)) {
                    uint8_t ka[4] = {0};
                    send(s->nb.sock, ka, 4, 0);
                    s->last_ka = now;
                }
            }

            int connecting = 0, bf_empty = 0;
            for (int i = 0; i < MAX_SESS; i++) {
                sess *s = &t->S[i];
                if (!s->active) continue;
                if (s->connecting) { connecting++; continue; }
                if (s->nb.handshaked && s->nb.bitfield) {
                    bool any = false;
                    for (size_t b = 0; b < s->nb.bitfield_len && !any; b++)
                        if (s->nb.bitfield[b]) any = true;
                    if (!any) bf_empty++;
                }
            }
            t->st_bf_empty = bf_empty;
            while (t->st_live < DIAL_STOP_LIVE && connecting < MAX_CONNECTING) {
                if (!sess_dial(t, now)) break;
                connecting++;
            }

            // Claims for idle unchoked sessions, within the calm budget.
            int claiming = 0;
            for (int i = 0; i < MAX_SESS; i++)
                if (t->S[i].active && t->S[i].claim >= 0) claiming++;
            for (int i = 0; i < MAX_SESS && claiming < t->calm_now; i++) {
                sess *s = &t->S[i];
                if (!s->active || s->connecting || !s->nb.handshaked) continue;
                if (s->nb.choked || s->claim >= 0) continue;
                claim_piece(t, s, i);
                if (s->claim >= 0) {
                    claiming++;
                    fill_pipeline(t, s);
                    peer_nb_flush(&s->nb);
                }
            }
        }

        // One poll over every session socket.
        struct pollfd pfd[MAX_SESS];
        int map[MAX_SESS];
        int n = 0;
        for (int i = 0; i < MAX_SESS; i++) {
            sess *s = &t->S[i];
            if (!s->active || s->nb.sock < 0) continue;
            pfd[n].fd = s->nb.sock;
            pfd[n].events = s->connecting
                                ? POLLOUT
                                : (short)(POLLIN | (peer_nb_tx_pending(&s->nb)
                                                        ? POLLOUT : 0));
            pfd[n].revents = 0;
            map[n] = i;
            n++;
        }

        u64 lt0 = armGetSystemTick();
        int pr = poll(pfd, (nfds_t)n, 200);
        lat_add(t, LAT_POLL, lt0);
        if (pr <= 0) continue;

        now = armGetSystemTick();
        for (int k = 0; k < n; k++) {
            if (!pfd[k].revents) continue;
            sess *s = &t->S[map[k]];
            if (!s->active) continue;
            if (s->connecting) {
                if (pfd[k].revents & (POLLOUT | POLLERR | POLLHUP))
                    sess_connected(t, s, now);
                continue;
            }
            if (pfd[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                t->st_fetch_fail++;
                sess_close(t, s, true);
                continue;
            }
            if (pfd[k].revents & POLLIN)
                sess_service(t, s, map[k], now);
            if (s->active && (pfd[k].revents & POLLOUT))
                if (peer_nb_flush(&s->nb) != 0) sess_close(t, s, true);
        }
    }

    for (int i = 0; i < MAX_SESS; i++) sess_close(t, &t->S[i], false);
}

//-----------------------------------------------------------------------------
// Open / close
//-----------------------------------------------------------------------------

torrentfs *torrentfs_open(const char *source, const char *cache_path,
                          char *err, size_t errlen) {
    return torrentfs_open_file(source, cache_path, -1, err, errlen);
}

torrentfs *torrentfs_open_file(const char *source, const char *cache_path,
                               int file_index, char *err, size_t errlen) {
    torrentfs *t = calloc(1, sizeof(*t));
    if (!t) { snprintf(err, errlen, "out of memory"); return NULL; }

    for (int i = 0; i < AQ_MAX; i++) t->aq[i].idx = -1;
    for (int i = 0; i < MAX_SESS; i++) t->S[i].claim = -1;
    t->freq     = armGetSystemTickFreq();
    t->ram_mode = g_ram_stream != 0;   // latched for this torrent's lifetime

    peer_addr seed_peers[80];
    int seed_count = 0;

    int rc = strncmp(source, "magnet:", 7) == 0
                 ? torrent_load_magnet_peers(&t->meta, source, seed_peers,
                                             (int)(sizeof(seed_peers) /
                                                   sizeof(seed_peers[0])),
                                             &seed_count, err, errlen)
                 : torrent_load(&t->meta, source, err, errlen);
    if (rc != 0) {
        free(t);
        return NULL;
    }

    int fi = file_index;
    if (fi < 0 || fi >= t->meta.file_count) fi = torrent_largest_file(&t->meta);
    if (fi < 0) {
        snprintf(err, errlen, "torrent has no files");
        torrent_unload(&t->meta);
        free(t);
        return NULL;
    }
    t->stream_offset    = t->meta.files[fi].offset;
    t->stream_size      = t->meta.files[fi].length;
    t->file_first_piece = t->stream_offset / t->meta.piece_len;
    t->file_last_piece  =
        (t->stream_offset + t->stream_size - 1) / t->meta.piece_len;
    t->playhead_piece   = t->file_first_piece;

    t->blocks_per_piece =
        (int)((t->meta.piece_len + BLOCK_LEN - 1) / BLOCK_LEN);
    t->status = calloc(1, (size_t)t->meta.piece_count);

    int64_t n_aq = RAM_BUDGET / t->meta.piece_len;
    if (n_aq < AQ_MIN) n_aq = AQ_MIN;
    if (n_aq > AQ_MAX) n_aq = AQ_MAX;
    t->n_aq = (int)n_aq;
    bool aq_ok = true;
    for (int i = 0; i < t->n_aq; i++) {
        t->aq[i].have = calloc(1, (size_t)t->blocks_per_piece);
        t->aq[i].req  = calloc(1, (size_t)t->blocks_per_piece);
        if (!t->aq[i].have || !t->aq[i].req) aq_ok = false;
    }

    snprintf(t->cache_base, sizeof(t->cache_base), "%s", cache_path);
    mutexInit(&t->cache_lock);
    mutexInit(&t->lock);
    for (int i = 0; i < CACHE_MAX_CHUNKS; i++) t->chunks[i] = -1;
    t->calm_now = MAX_SESS;
    t->slots_per_chunk = CACHE_CHUNK / t->meta.piece_len;
    t->piece_slot = malloc((size_t)t->meta.piece_count * sizeof(int32_t));
    if (t->piece_slot)
        for (int64_t i = 0; i < t->meta.piece_count; i++) t->piece_slot[i] = -1;
    cache_delete_all(t);  // a previous run's leftovers (crash) are dead weight

    if (t->ram_mode) {
        t->ram_budget = RAM_STREAM_BUDGET;
        t->ram_lo     = t->file_first_piece;
        t->ram_piece  = calloc((size_t)t->meta.piece_count, sizeof(uint8_t *));
    }
    // The SD cache is only needed when not streaming into RAM: skip its file so
    // RAM mode touches the card zero times.
    bool cache_ok = t->ram_mode
                        ? (t->ram_piece != NULL)
                        : (t->piece_slot && t->slots_per_chunk >= 1 &&
                           cache_chunk(t, 0) >= 0);
    if (!t->status || !aq_ok || !cache_ok) {
        snprintf(err, errlen, "cache/status alloc failed");
        torrentfs_close(t);
        return NULL;
    }

    memcpy(t->peer_id, "-SW0003-", 8);
    srand((unsigned)time(NULL));
    for (int i = 8; i < 20; i++) t->peer_id[i] = (uint8_t)(rand() % 256);

    if (seed_count > 0) add_peers_cb(t, seed_peers, seed_count);

    if (threadCreate(&t->discovery, discovery_main, t, NULL, 0x20000, 0x2C,
                     -2) == 0) {
        t->discovery_started = true;
        threadStart(&t->discovery);
    }
    // Priority 0x2D, one notch BELOW the app default (0x2C): the writer runs
    // on the render thread's default core, and Horizon neither timeslices nor
    // preempts between equal priorities -- at 0x2C, a render thread waking
    // from vsync waited out whatever slice of a 4 MB SD write (or the SHA-1)
    // the writer had started, and the playback draw loop has zero frame
    // budget to spare. One notch lower, the renderer preempts it instead.
    // The writer needs ~1 write per streamed piece; it starves only if every
    // core is 100% busy, which hardware-decoded playback never sustains.
    if (threadCreate(&t->writer, writer_main, t, NULL, 0x20000, 0x2D, -2) == 0) {
        t->writer_started = true;
        threadStart(&t->writer);
    } else {
        snprintf(err, errlen, "could not start the writer thread");
        torrentfs_close(t);
        return NULL;
    }
    // Priority 0x2B, one notch above the app default: Horizon never preempts
    // between equal priorities, and a CPU-bound mpv thread otherwise starves
    // this loop's ~100 IPC yield points for seconds at a time (v2's finding).
    if (threadCreate(&t->netloop, netloop_main, t, NULL, 0x40000, 0x2B, -2) == 0) {
        t->netloop_started = true;
        threadStart(&t->netloop);
    } else {
        snprintf(err, errlen, "could not start the network thread");
        torrentfs_close(t);
        return NULL;
    }

    return t;
}

void torrentfs_close(torrentfs *tfs) {
    if (!tfs) return;
    tfs->stop = true;

    if (tfs->discovery_started) {
        threadWaitForExit(&tfs->discovery);
        threadClose(&tfs->discovery);
        tfs->discovery_started = false;
    }
    if (tfs->netloop_started) {
        threadWaitForExit(&tfs->netloop);
        threadClose(&tfs->netloop);
        tfs->netloop_started = false;
    }
    // The writer drains its queue after stop; join it after the netloop
    // (which can still push jobs).
    if (tfs->writer_started) {
        threadWaitForExit(&tfs->writer);
        threadClose(&tfs->writer);
        tfs->writer_started = false;
    }
    for (int i = 0; i < CACHE_MAX_CHUNKS; i++)
        if (tfs->chunks[i] >= 0) { close(tfs->chunks[i]); tfs->chunks[i] = -1; }
    cache_delete_all(tfs);  // per-playback scratch, not a library
    for (int i = 0; i < AQ_MAX; i++) {
        free(tfs->aq[i].buf);
        free(tfs->aq[i].have);
        free(tfs->aq[i].req);
    }
    if (tfs->ram_piece) {   // free before torrent_unload: needs meta.piece_count
        for (int64_t i = 0; i < tfs->meta.piece_count; i++) free(tfs->ram_piece[i]);
        free(tfs->ram_piece);
    }
    free(tfs->status);
    free(tfs->piece_slot);
    torrent_unload(&tfs->meta);
    free(tfs);
}

//-----------------------------------------------------------------------------
// The player-facing read path (mpv demuxer thread). LOCK-FREE on purpose:
// Horizon does not timeslice equal-priority threads, so a CPU-bound mpv
// thread can starve this one for whole seconds -- if it held an engine lock
// at that moment, the netloop would freeze with it. An aligned 64-bit store
// is atomic on AArch64.
//-----------------------------------------------------------------------------

int64_t torrentfs_size(const torrentfs *tfs) {
    return tfs->stream_size;
}

void torrentfs_set_playhead(torrentfs *tfs, int64_t offset) {
    int64_t abs = tfs->stream_offset + offset;
    ((torrentfs *)tfs)->playhead_piece = abs / tfs->meta.piece_len;
}

static bool have_piece(torrentfs *t, int64_t idx) {
    // DONE is terminal and only set once the piece is fully on disk, so the
    // worst a stale read costs is one extra 20 ms wait.
    return t->status[idx] == PIECE_DONE;
}

int64_t torrentfs_read(torrentfs *tfs, int64_t offset, char *buf, int64_t nbytes) {
    hb_beat(tfs, HB_READER);
    if (offset >= tfs->stream_size) return 0;
    if (offset + nbytes > tfs->stream_size) nbytes = tfs->stream_size - offset;
    if (nbytes <= 0) return 0;

    torrentfs_set_playhead(tfs, offset);

    int64_t abs       = tfs->stream_offset + offset;
    int64_t abs_total = tfs->stream_offset + tfs->stream_size;
    int64_t plen      = tfs->meta.piece_len;
    int64_t first     = abs / plen;

    while (!tfs->stop && !have_piece(tfs, first))
        svcSleepThread(20000000ULL);  // 20 ms
    if (tfs->stop) return -1;

    int64_t last_byte  = abs + nbytes - 1;
    int64_t last_piece = last_byte / plen;
    int64_t avail_end  = (first + 1) * plen;
    for (int64_t pc = first + 1; pc <= last_piece; pc++) {
        if (!have_piece(tfs, pc)) break;
        avail_end = (pc + 1) * plen;
    }
    if (avail_end > abs_total) avail_end = abs_total;

    int64_t can_read = avail_end - abs;
    if (can_read > nbytes) can_read = nbytes;

    mutexLock(&tfs->cache_lock);
    u64 lt0 = armGetSystemTick();
    size_t got = cache_read_upto(tfs, abs, buf, (size_t)can_read);
    lat_add(tfs, LAT_RD, lt0);
    mutexUnlock(&tfs->cache_lock);

    // Racy on purpose: diagnostic counter on the mpv thread.
    if ((int64_t)got < can_read) ((torrentfs *)tfs)->st_cache_rd_short++;
    // Short reads are legal for mpv; an error would kill the stream for good.
    return (int64_t)got;
}

void torrentfs_cancel(torrentfs *tfs) {
    tfs->stop = true;
}

//-----------------------------------------------------------------------------
// Debug surface
//-----------------------------------------------------------------------------

void torrentfs_stats(const torrentfs *tfs, int64_t *pieces_done,
                     int64_t *pieces_total, int64_t *playhead_piece) {
    mutexLock((Mutex *)&tfs->lock);
    if (pieces_done) *pieces_done = tfs->pieces_done;
    if (pieces_total)
        *pieces_total = tfs->file_last_piece - tfs->file_first_piece + 1;
    if (playhead_piece) *playhead_piece = tfs->playhead_piece;
    mutexUnlock((Mutex *)&tfs->lock);
}

int torrentfs_peer_count(const torrentfs *tfs) {
    return tfs->peer_count;
}

void torrentfs_live_peers(const torrentfs *tfs, int *live, int *peak,
                          int *connecting) {
    if (live) *live = tfs->st_live;
    if (peak) *peak = tfs->st_peak_live;
    if (connecting) *connecting = tfs->st_connecting;
}

void torrentfs_claim_stats(const torrentfs *tfs, int *claiming, int *idle) {
    int c = 0, id = 0;
    for (int i = 0; i < MAX_SESS; i++) {
        const sess *s = &tfs->S[i];
        if (!s->active || s->connecting || !s->nb.handshaked) continue;
        if (s->claim >= 0) c++;
        else if (!s->nb.choked) id++;
    }
    if (claiming) *claiming = c;
    if (idle) *idle = id;
}

void torrentfs_piece_debug(const torrentfs *tfs, int64_t idx, int *status,
                           int *have, int *req, int *total) {
    torrentfs *t = (torrentfs *)tfs;
    int h = 0, r = 0, nb = 0, st = -1;
    if (idx >= 0 && idx < t->meta.piece_count) {
        st = t->status[idx];
        nb = (int)((torrent_piece_len(&t->meta, idx) + BLOCK_LEN - 1) / BLOCK_LEN);
        aq_entry *a = aq_find(t, idx);   // racy vs the netloop: diagnostics
        if (a) {
            h = a->have_cnt;
            for (int b = 0; b < a->nblocks; b++)
                if (a->req[b] && !a->have[b]) r++;
        }
    }
    if (status) *status = st;
    if (have) *have = h;
    if (req) *req = r;
    if (total) *total = nb;
}

int64_t torrentfs_stored_bytes(const torrentfs *tfs) {
    mutexLock((Mutex *)&tfs->lock);
    int64_t v = tfs->pieces_done * tfs->meta.piece_len +
                tfs->st_blocks_have * BLOCK_LEN;
    mutexUnlock((Mutex *)&tfs->lock);
    return v;
}

void torrentfs_cache_stats(const torrentfs *tfs, int *wr_fail, int *rd_short,
                           int64_t *total_bytes) {
    if (wr_fail) *wr_fail = tfs->st_cache_wr_fail;
    if (rd_short) *rd_short = tfs->st_cache_rd_short;
    if (total_bytes)
        *total_bytes = (tfs->file_last_piece - tfs->file_first_piece + 1) *
                       tfs->meta.piece_len;
}

int64_t torrentfs_cache_written(const torrentfs *tfs) {
    return tfs->st_cache_written;
}

void torrentfs_set_backlog(torrentfs *tfs, int ms) {
    tfs->backlog_ms = ms;
}

int torrentfs_calm(const torrentfs *tfs) {
    return tfs->calm_now;
}

void torrentfs_hb_ui(torrentfs *tfs) {
    hb_beat(tfs, HB_UI);
}

void torrentfs_heartbeats(const torrentfs *tfs, uint32_t age_ms[4],
                          int core[4]) {
    u64 now = armGetSystemTick();
    for (int i = 0; i < 4; i++) {
        u64 tk    = tfs->hb_tick[i];
        age_ms[i] = tk ? (uint32_t)((now - tk) * 1000 / tfs->freq) : 0;
        core[i]   = tfs->hb_core[i];
    }
}

void torrentfs_lat_stats(const torrentfs *tfs, uint32_t count[5],
                         uint64_t max_us[5]) {
    torrentfs *t = (torrentfs *)tfs;
    for (int i = 0; i < 5; i++) {
        count[i]  = t->lat_n[i];
        max_us[i] = t->lat_max[i] * 1000000 / t->freq;
        t->lat_max[i] = 0;   // reading clears the peaks
    }
}

void torrentfs_claim_debug(const torrentfs *tfs, int64_t *ph, int64_t *lo,
                           int64_t *hi, int *fail, int *ok, int *inflight) {
    if (ph) *ph = tfs->st_win_ph;
    if (lo) *lo = tfs->st_win_lo;
    if (hi) *hi = tfs->st_win_hi;
    if (fail) *fail = tfs->st_claim_fail;
    if (ok) *ok = tfs->st_claim_ok;
    if (inflight) {
        int n = 0;
        for (int i = 0; i < tfs->n_aq; i++)
            if (tfs->aq[i].idx >= 0) n++;
        *inflight = n;
    }
}

void torrentfs_bitfield_stats(const torrentfs *tfs, int *empty, int *ok, int *bad) {
    // st_bf_empty is computed by the netloop's upkeep tick: walking the live
    // bitfields here (UI thread) would race peer_nb_free.
    if (empty) *empty = tfs->st_bf_empty;
    if (ok) *ok = tfs->st_bf_ok;
    if (bad) *bad = tfs->st_bf_bad;
}

void torrentfs_fail_kinds(const torrentfs *tfs, int *sock_fail, int *timeouts) {
    if (sock_fail) *sock_fail = tfs->st_sock_fail;
    if (timeouts) *timeouts = tfs->st_conn_timeout;
}

void torrentfs_debug_counts(const torrentfs *tfs, int out[10]) {
    out[0] = tfs->st_conn_ok;
    out[1] = tfs->st_conn_fail;
    out[2] = tfs->st_unchoke_ok;
    out[3] = tfs->st_choked;
    out[4] = tfs->st_piece_ok;
    out[5] = tfs->st_fetch_fail;
    out[6] = tfs->st_sha_fail;
    out[7] = 0;   // blocks_served: v3 is leech-only
    out[8] = tfs->st_interested_recv;
    out[9] = tfs->st_request_recv;
}

int64_t torrentfs_piece_len(const torrentfs *tfs) {
    return tfs->meta.piece_len;
}

const char *torrentfs_name(const torrentfs *tfs) {
    return tfs->meta.name[0] ? tfs->meta.name : "torrent";
}

int torrentfs_piece_done(const torrentfs *tfs, int64_t idx) {
    if (idx < 0 || idx >= tfs->meta.piece_count) return 0;
    return tfs->status[idx] == PIECE_DONE;
}

int torrentfs_incoming_count(const torrentfs *tfs) {
    (void)tfs;
    return 0;   // leech-only: no listen socket
}

int64_t torrentfs_bytes_recv(const torrentfs *tfs) {
    return tfs->st_bytes_recv;
}

void torrentfs_last_err(const torrentfs *tfs, char *buf, size_t len) {
    snprintf(buf, len, "%s", tfs->st_last_err);
}
