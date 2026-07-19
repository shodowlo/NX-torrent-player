// torrentfs2.c — v2 of the streaming torrent engine (same public API as v1).
//
// Rebuilt around what capped v1's throughput in practice:
//
//  1. RAM piece assembly. v1 wrote every 16 KB block to the SD card as it
//     arrived (256 fseek+fwrite per 4 MB piece, under a lock the player's
//     reads also need), then re-read the whole piece from SD to verify it.
//     SD traffic per piece was ~3x the piece size and all of it serialized
//     against playback. v2 assembles pieces in RAM buffers, verifies the
//     SHA-1 from RAM, and hands the finished piece to a writer thread that
//     does ONE sequential write. The engine's SD traffic per piece is now
//     exactly the piece size, off the hot path.
//
//  2. Adaptive pipelining. v1 kept a fixed 24 blocks in flight per peer:
//     384 KB, which caps a 100 ms-RTT peer at ~4 MB/s and yet over-reserves
//     against slow peers. v2 measures each peer's delivery rate and sizes its
//     window to ~1.5 s of it (8..192 blocks), so fast seeds stay saturated
//     and slow peers hold few reservations.
//
//  3. A global block scheduler instead of one-piece-per-session. Sessions
//     pull "the next most-urgent blocks this peer has" straight off the
//     shared priority order (startup head/tail, then playhead window, then
//     backfill), so pipelines never drain at piece boundaries and every
//     session automatically piles onto whatever the player is blocked on.
//
//  4. Peer lifecycle that keeps peers. v1 reaped any silent peer after 20 s —
//     but a choking peer is silent by design (keep-alives are 120 s apart,
//     optimistic unchokes rotate every ~30 s), so v1 dropped and re-dialed
//     the same seeders in a loop, which real clients punish with bans. v2
//     keeps handshaked peers 130 s, sends its own keep-alives, and spaces
//     reconnects with a per-peer backoff.
//
// Kept from v1, because they were hard-won on this platform: the single
// poll() session loop (libnx allows 16 concurrent *blocking* BSD calls
// total), the ~48-session cap (socket buffer pool), the FAT32-chunked cache,
// the startup head/tail critical pieces, the bounded streaming window, the
// stalled-piece racing, upload serving for tit-for-tat, incoming acceptors,
// and periodic tracker/DHT discovery with pool eviction.

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
#include <sys/time.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <mbedtls/sha1.h>

#include "torrent.h"
#include "peer.h"      // MSG_* ids, BLOCK_LEN, MAX_MSG_LEN, bf_has_piece
#include "dhtclient.h"

//-----------------------------------------------------------------------------
// Tuning
//-----------------------------------------------------------------------------

// Session budget against the 16-session blocking-BSD pool: 1 netloop poll()
// + 2 acceptors in accept() + tracker/DHT = ~5. Peer sessions cost none.
#define ACCEPTORS        2
#define INQ              32     // accepted fds waiting for the loop
#define LISTEN_PORT      6881   // must match what udp_tracker.c announces

#define MAX_SESS         48     // bounded by the socket *buffer* pool, not fds
#define MAX_CONNECTING   24     // slots allowed to sit in a pending connect
#define DIAL_STOP_LIVE   24     // enough live sessions: stop dialing new peers.
                                // Without this the loop kept retrying the dead
                                // majority of the swarm forever (~3.5 SYN/s,
                                // 426 timeouts in a 100 s log) -- constant
                                // radio and bsd noise bought with nothing.
#define CONNECT_SECS     2      // outbound SYN patience (parallel, so cheap)
#define PREHS_SECS       10     // connected but no handshake yet
#define IDLE_SECS        130    // handshaked, nothing at all received. Must sit
                                // ABOVE the 120 s keep-alive interval, or we
                                // reap every choking peer before its unchoke
                                // rotation reaches us (v1's mistake).
#define KEEPALIVE_SECS   60     // how often WE prove liveness to the peer
#define STALL_SECS       5      // reservations of a non-delivering peer expire

// Adaptive pipeline depth, in blocks: ~1.5 s of the peer's measured rate.
#define DEPTH_START      24
#define DEPTH_MIN        8
#define DEPTH_MAX        192

// Inbound buffer per session. Every recv() is an IPC to the bsd service, so
// draining more per call directly cuts CPU per MB. 64 KB ~= 4 blocks.
#define RX_CAP           (64 * 1024)
#define TX_MAX           (512 * 1024)  // upload backlog cap per peer

// RAM given to in-flight piece buffers. Piece assembly lives here; the pool
// bounds how many pieces can be open at once (4..32 whatever the piece size).
#define RAM_BUDGET       (64LL << 20)
#define AP_MAX           32
#define AP_MIN           4

// Streaming window (see v1's rationale, kept verbatim): wide enough to feed
// the sessions, narrow enough that they don't advance 50 pieces in parallel
// while the player starves on one.
#define STREAM_WINDOW    (32 * 1024 * 1024)
#define STREAM_MIN_PIECES 16

// Startup-critical pieces: first CRIT_HEAD + last CRIT_TAIL of the file (moov
// atom either end). Fetched redundantly until the playhead moves past the head.
#define CRIT_HEAD        2
#define CRIT_TAIL        3

// FAT32 caps a file at 4 GB: the cache is chunked.
#define CACHE_CHUNK      (1LL << 30)
#define CACHE_MAX_CHUNKS 64
// Piece writes release cache_lock between slices of this size, so a slow SD
// write never makes the player's reads wait out the whole piece.
#define WR_SLICE         (256 * 1024)

#define TFS_MAX_PEERS    256

// Per-peer reconnect backoff (v1 fix, kept): failed connects double 15..300 s;
// a peer that had connected gets a flat 30 s so we don't reconnect-hammer it.
#define BACKOFF_CONN_SECS 15
#define BACKOFF_MAX_SECS  300
#define BACKOFF_DROP_SECS 30

// Periodic discovery (v1 fix, kept): trackers+DHT re-run every 15 min, or
// 60 s after the previous round when almost no session is alive.
#define DISC_INTERVAL_SECS (15 * 60)
#define DISC_STARVED_SECS  60
#define STARVED_LIVE       3

enum { PIECE_NEEDED = 0, PIECE_ACTIVE = 1, PIECE_DONE = 2, PIECE_WRITING = 3 };

// Fill-rate governor toggle (torrentfs_set_governor): read at use sites so
// flipping the Options switch takes effect immediately, mid-playback included.
// Default matches the config default (on).
static volatile int g_governor = 1;

void torrentfs_set_governor(int on) { g_governor = on ? 1 : 0; }
int  torrentfs_governor(void)       { return g_governor; }

//-----------------------------------------------------------------------------
// Types
//-----------------------------------------------------------------------------

// One piece being assembled in RAM. Slots (and their lazily-allocated buffers)
// are reused; `own[b]` is the session id + 1 holding the only reservation on
// block b, `res_cnt` how many blocks are reserved. Mutated by the netloop
// under t->lock (the UI reads these for the debug panel).
typedef struct {
    int64_t idx;        // -1 = slot free
    uint8_t *buf;       // piece_len bytes, lazily allocated, reused
    uint8_t *have;      // one byte per block
    uint8_t *own;       // 0 = unreserved, else sess id + 1
    int nblocks;
    int have_cnt;
    int res_cnt;
} apiece;

// An outstanding block request of one session.
typedef struct {
    int32_t piece;
    uint16_t block;
} oreq;

typedef struct {
    int sid;              // index in the session table (owner byte = sid + 1)
    int sock;
    bool active, dead, connecting, handshaked;
    bool choked;          // peer chokes us
    bool we_unchoked;     // we unchoked the peer
    bool counted_unchoke;
    int pidx;             // peer pool index, -1 for incoming

    uint8_t *rx;          // RX_CAP
    size_t rx_off, rx_len;
    uint8_t *tx;
    size_t tx_head, tx_len, tx_cap;

    uint8_t *bitfield;    // peer's pieces
    uint8_t *adv;         // pieces we've advertised to it

    oreq out[DEPTH_MAX];  // our outstanding block requests
    int out_n;
    int depth;            // current pipeline target
    int rate_blocks;      // blocks landed since last_rate
    double rate_ewma;     // blocks per second
    u64 last_rate;

    u64 started, last_rx, last_tx, last_block, last_adv;
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
    int blocks_per_piece;   // of a full piece
    size_t bf_len;          // bitfield bytes for piece_count

    // FAT32-chunked cache, raw fds (-1 = not open). cache_lock serializes the
    // writer thread's piece writes, the player's reads and upload-serving
    // reads — all of them whole-buffer operations now, so contention is rare
    // and short. Pieces are stored append-only; piece_slot maps a torrent
    // piece to the slot it landed in (-1 = not stored). See cache_piece_io.
    int chunks[CACHE_MAX_CHUNKS];    // one fd per chunk. Horizon's FS refuses
                                     // a second write-mode open of the same
                                     // file, so writer and readers share
                                     // these under cache_lock.
    char cache_base[192];
    Mutex cache_lock;
    int32_t *piece_slot;     // piece index -> cache slot, -1 = not stored
    int64_t next_slot;       // next append slot
    int64_t slots_per_chunk; // CACHE_CHUNK / piece_len (floor)

    // t->lock guards: status[], piece_stalled[], the active-piece table's
    // shared fields, the peer pool, the writer queue, playhead and stats.
    Mutex lock;
    uint8_t *status;
    uint8_t *piece_stalled;
    int64_t pieces_done;        // pieces DONE (only file pieces ever download)
    int64_t playhead_piece;
    volatile bool stop;

    apiece ap[AP_MAX];
    int n_ap;                   // usable slots (RAM budget / piece_len)

    // Writer: finished (verified) pieces travel netloop -> writer -> SD.
    wjob wq[AP_MAX + 2];
    int wq_head, wq_n;
    Thread writer;
    bool writer_started;

    // Peer pool + discovery (v1 fixes kept: backoff, eviction, periodic).
    peer_addr peers[TFS_MAX_PEERS];
    uint8_t peer_fails[TFS_MAX_PEERS];
    uint8_t peer_busy[TFS_MAX_PEERS];
    u64 peer_next_try[TFS_MAX_PEERS];
    int peer_count;
    int next_peer;

    uint8_t peer_id[20];

    int listen_sock;
    Thread acceptors[ACCEPTORS];
    int n_acceptors;
    int incoming[INQ];

    Thread netloop;
    bool netloop_started;
    sess *S;                    // valid while the netloop runs (cancel scans)

    Thread announce_thread, dht_thread;
    bool announce_started, dht_started;

    u64 freq;

    // Stats (guarded by lock), matching the v1 debug surface.
    int st_conn_ok, st_conn_fail, st_unchoke_ok, st_choked;
    int st_piece_ok, st_fetch_fail, st_sha_fail;
    int st_blocks_served, st_incoming;
    int64_t st_bytes_recv;
    int64_t st_blocks_have;     // blocks resident in RAM pieces (partials)
    int st_interested_recv, st_request_recv;
    int st_sock_fail, st_conn_timeout, st_connecting;
    int st_claiming, st_idle_unchoked, st_bf_empty, st_bf_ok, st_bf_bad;
    int64_t st_win_ph, st_win_lo, st_win_hi;
    int st_claim_fail, st_claim_ok;
    int st_cache_wr_fail, st_cache_rd_short;
    int64_t st_cache_written;   // bytes successfully written to the SD cache

    // Syscall latency probes (torrentfs_lat_stats): when the console freezes,
    // whichever class's peak explodes names the OS service that stalled (bsd
    // for poll/recv/send, fs for wr/rd). Each class has exactly one writer
    // thread; the UI reads (and clears peaks) racily, which is fine for a
    // diagnostic.
    uint32_t lat_n[5];    // calls, cumulative
    uint64_t lat_max[5];  // peak ticks for one call, since last read

    // Thread heartbeats (torrentfs_heartbeats): each probe thread stamps the
    // tick and the core it is running on. During a freeze, the stats line
    // then shows WHICH threads stopped being scheduled and on WHICH core the
    // survivors ran -- syscall probes proved the stalled threads are not
    // stuck in syscalls, so the question is core starvation topology.
    u64 hb_tick[4];
    u8  hb_core[4];

    // Burst smoothing (torrentfs_set_backlog): the player reports how many
    // seconds it has buffered; the deeper the backlog, the fewer sessions may
    // claim new work (calm_budget). See the calm block in netloop_main.
    int backlog_ms;       // written by the app thread, read racily
    int calm_now;         // sessions currently allowed to claim (panel)
    uint8_t claim_allowed[MAX_SESS];
    // Fill-rate governor (optional, g_governor): session count alone cannot
    // smooth anything -- ONE fast peer saturates the wifi (the OS-core cost
    // is packets/s, i.e. bytes/s, regardless of peer count). When the backlog
    // is deep, claims pause whenever the measured rate exceeds a
    // backlog-dependent target.
    int64_t calm_bps;     // 0 = unlimited
    double  rate_bps;     // download rate EWMA (netloop-only)
    int64_t rate_last_bytes;
    u64     rate_last_tick;
    int st_live, st_peak_live;
    char st_last_err[128];
};

static void inc(torrentfs *t, int *c) {
    mutexLock(&t->lock);
    (*c)++;
    mutexUnlock(&t->lock);
}

enum { LAT_POLL = 0, LAT_RECV, LAT_SEND, LAT_WR, LAT_RD };

static void lat_add(torrentfs *t, int c, u64 t0) {
    u64 d = armGetSystemTick() - t0;
    t->lat_n[c]++;
    if (d > t->lat_max[c]) t->lat_max[c] = d;
}

enum { HB_NET = 0, HB_WRITER = 1, HB_READER = 2, HB_UI = 3 };

static void hb_beat(torrentfs *t, int k) {
    t->hb_tick[k] = armGetSystemTick();
    t->hb_core[k] = (u8)svcGetCurrentProcessorNumber();
}

static uint32_t block_len_of(int64_t plen, int b) {
    int64_t rem = plen - (int64_t)b * BLOCK_LEN;
    return rem > BLOCK_LEN ? BLOCK_LEN : (uint32_t)rem;
}

//-----------------------------------------------------------------------------
// Chunked cache I/O (FAT32-safe). Callers hold cache_lock.
//
// Pieces are stored COMPACTLY, in arrival order, at the next free slot -- NOT
// at their torrent offset. FAT has no sparse files: a write past EOF makes the
// fs sysmodule allocate and journal every cluster in between, on the OS core
// (core 3), while we sit inside the write holding cache_lock. The old layout
// (slot = torrent offset) hit that constantly: the startup moov-tail pieces
// landed up to 1 GB deep in a fresh chunk file, a mid-film seek likewise --
// tens of seconds of console-wide freeze (player reads block on cache_lock,
// the writer stalls, the RAM piece pool drains, the download collapses to
// zero). Appending keeps every write within one piece of EOF, so the only
// clusters ever allocated are the piece's own. piece_slot[] remembers where
// each piece went; the mapping dies with the session (the cache is deleted on
// open and close anyway).
//
// Raw fds, not stdio: newlib gives a FILE* a ~1 KB buffer and moves fread/
// fwrite through it in buffer-sized slices -- thousands of fs IPCs per piece.
// read()/write() hand the whole buffer to the fs service in one call.
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

// Full transfer at fd+offset; loops on short counts.
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

// Locate piece `idx`: chunk + byte offset of its slot. A write's first touch
// appends a new slot; a read of an unstored piece is a miss.
static bool piece_loc(torrentfs *t, int64_t idx, bool alloc, int *ci,
                      int64_t *co) {
    int32_t slot = t->piece_slot[idx];
    if (slot < 0) {
        if (!alloc) return false;
        if (t->next_slot >= (int64_t)CACHE_MAX_CHUNKS * t->slots_per_chunk)
            return false;  // cache full (64 chunks) -- should not happen
        slot = (int32_t)t->next_slot++;
        t->piece_slot[idx] = slot;
    }
    *ci = (int)(slot / t->slots_per_chunk);
    *co = (int64_t)(slot % t->slots_per_chunk) * t->meta.piece_len;
    return true;
}

// Read [within, within+len) of piece `idx`. Reads only ever target pieces
// that were written in full (status DONE), so short data is impossible.
// Caller holds cache_lock (serializes the readers sharing the reader fds).
static bool cache_piece_read(torrentfs *t, int64_t idx, int64_t within,
                             void *buf, size_t len) {
    int ci;
    int64_t co;
    if (!piece_loc(t, idx, false, &ci, &co)) return false;
    int f = cache_chunk(t, ci);
    if (f < 0) return false;
    return fd_xfer(f, co + within, buf, len, false);
}

// Flush only the chunks a [off, off+len) write touched.
// The cache is scratch space for the current playback, not a library: it can
// reach the size of the whole video, so leaving it behind wastes SD space for
// data the app will re-download anyway (piece state never survives a reopen).
// Deleted on close, and on open too so a crash's leftovers don't linger.
static void cache_delete_all(torrentfs *t) {
    for (int i = 0; i < CACHE_MAX_CHUNKS; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s.%03d", t->cache_base, i);
        remove(path);  // fails harmlessly on chunks that were never created
    }
}

// Read as much as is there; the player path must never hard-fail (mpv treats
// an error as a dead stream and stops reading for good). `off` is an absolute
// torrent offset; each spanned piece is looked up through its slot.
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
// Session tx/rx (non-blocking, self-contained)
//-----------------------------------------------------------------------------

static int tx_append(sess *s, const void *buf, size_t len) {
    if (s->tx_head > 0 && s->tx_head == s->tx_len)
        s->tx_head = s->tx_len = 0;
    if (s->tx_len + len > s->tx_cap) {
        if (s->tx_head > 0) {
            memmove(s->tx, s->tx + s->tx_head, s->tx_len - s->tx_head);
            s->tx_len -= s->tx_head;
            s->tx_head = 0;
        }
        while (s->tx_len + len > s->tx_cap) {
            size_t cap = s->tx_cap * 2;
            if (cap > TX_MAX) return -1;  // peer never drains: don't balloon
            uint8_t *nt = realloc(s->tx, cap);
            if (!nt) return -1;
            s->tx = nt;
            s->tx_cap = cap;
        }
    }
    memcpy(s->tx + s->tx_len, buf, len);
    s->tx_len += len;
    s->last_tx = armGetSystemTick();
    return 0;
}

static int queue_msg(sess *s, uint8_t id, const void *payload, uint32_t plen) {
    uint8_t hdr[5];
    uint32_t len = htonl(1 + plen);
    memcpy(hdr, &len, 4);
    hdr[4] = id;
    if (tx_append(s, hdr, 5) != 0) return -1;
    if (plen && tx_append(s, payload, plen) != 0) return -1;
    return 0;
}

static void queue_req(sess *s, uint8_t id, int64_t piece, uint32_t begin,
                      uint32_t len) {
    uint8_t req[12];
    uint32_t v;
    v = htonl((uint32_t)piece); memcpy(req + 0, &v, 4);
    v = htonl(begin);           memcpy(req + 4, &v, 4);
    v = htonl(len);             memcpy(req + 8, &v, 4);
    queue_msg(s, id, req, 12);
}

static int tx_flush(sess *s) {
    while (s->tx_head < s->tx_len) {
        ssize_t n = send(s->sock, s->tx + s->tx_head, s->tx_len - s->tx_head, 0);
        if (n > 0) { s->tx_head += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1;
    }
    s->tx_head = s->tx_len = 0;
    return 0;
}

static bool tx_pending(const sess *s) { return s->tx_head < s->tx_len; }

static void rx_compact(sess *s) {
    if (s->rx_off == 0) return;
    if (s->rx_len > s->rx_off)
        memmove(s->rx, s->rx + s->rx_off, s->rx_len - s->rx_off);
    s->rx_len -= s->rx_off;
    s->rx_off = 0;
}

// Drain the socket. Returns bytes read, or -1 on EOF/error.
static ssize_t rx_pump(sess *s) {
    ssize_t total = 0;
    for (;;) {
        if (s->rx_len >= RX_CAP) {
            rx_compact(s);
            if (s->rx_len >= RX_CAP) return total;  // caller must eat messages
        }
        size_t want = RX_CAP - s->rx_len;
        ssize_t n = recv(s->sock, s->rx + s->rx_len, want, 0);
        if (n > 0) {
            s->rx_len += (size_t)n;
            total += n;
            // A short return means the kernel buffer is drained: skip the
            // recv() that would only report EAGAIN. Every recv is an IPC to
            // the bsd sysmodule (serviced on the OS core), so at streaming
            // rates that confirmation call doubled the per-socket bill.
            if ((size_t)n < want) return total;
            continue;
        }
        if (n == 0) return -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return total;
        return -1;
    }
}

// Pop one message (skipping keep-alives): 1 ok, 0 not whole yet, -1 illegal.
static int rx_next(sess *s, uint8_t *id, uint8_t **payload, uint32_t *plen) {
    for (;;) {
        size_t avail = s->rx_len - s->rx_off;
        if (avail < 4) { rx_compact(s); return 0; }
        uint32_t len_be;
        memcpy(&len_be, s->rx + s->rx_off, 4);
        uint32_t len = ntohl(len_be);
        if (len == 0) { s->rx_off += 4; continue; }  // keep-alive
        if (len > MAX_MSG_LEN) return -1;
        if (avail < 4 + (size_t)len) { rx_compact(s); return 0; }
        *id      = s->rx[s->rx_off + 4];
        *plen    = len - 1;
        *payload = s->rx + s->rx_off + 5;
        s->rx_off += 4 + (size_t)len;
        return 1;
    }
}

#define HANDSHAKE_LEN 68

static void queue_handshake(sess *s, const uint8_t info_hash[20],
                            const uint8_t peer_id[20]) {
    uint8_t hs[HANDSHAKE_LEN];
    hs[0] = 19;
    memcpy(hs + 1, "BitTorrent protocol", 19);
    memset(hs + 20, 0, 8);
    memcpy(hs + 28, info_hash, 20);
    memcpy(hs + 48, peer_id, 20);
    tx_append(s, hs, HANDSHAKE_LEN);
}

// 1 = done, 0 = waiting, -1 = wrong protocol/torrent.
static int rx_handshake(sess *s, const uint8_t info_hash[20]) {
    if (s->rx_len - s->rx_off < HANDSHAKE_LEN) { rx_compact(s); return 0; }
    const uint8_t *hs = s->rx + s->rx_off;
    if (hs[0] != 19 || memcmp(hs + 1, "BitTorrent protocol", 19) != 0) return -1;
    if (memcmp(hs + 28, info_hash, 20) != 0) return -1;
    s->rx_off += HANDSHAKE_LEN;
    s->handshaked = true;
    rx_compact(s);
    return 1;
}

//-----------------------------------------------------------------------------
// Active pieces + scheduler
//-----------------------------------------------------------------------------

// Netloop is the only mutator of the ap table; the UI only reads it (under
// t->lock). ap_find may run without the lock on the netloop thread.
static apiece *ap_find(torrentfs *t, int64_t idx) {
    for (int i = 0; i < t->n_ap; i++)
        if (t->ap[i].idx == idx) return &t->ap[i];
    return NULL;
}

static bool startup_phase(torrentfs *t, int64_t ph, int64_t lo) {
    (void)t;
    return ph <= lo + CRIT_HEAD;
}

static bool is_startup_crit(torrentfs *t, int64_t idx, int64_t ph, int64_t lo,
                            int64_t fhi) {
    if (!startup_phase(t, ph, lo)) return false;
    return (idx < lo + CRIT_HEAD) || (idx > fhi - CRIT_TAIL);
}

// Find or open a RAM slot for piece `idx`. Caller holds t->lock. May evict an
// idle active piece that fell out of the hot window (a seek strands partial
// pieces behind the playhead; without eviction they'd pin every buffer and
// freeze the scheduler for good).
static apiece *ap_get(torrentfs *t, int64_t idx, int64_t ph, int64_t hi,
                      int64_t lo, int64_t fhi) {
    apiece *a = ap_find(t, idx);
    if (a) return a;

    int slot = -1;
    for (int i = 0; i < t->n_ap; i++)
        if (t->ap[i].idx < 0) { slot = i; break; }

    if (slot < 0) {
        int victim = -1;
        int64_t worst = -1;
        for (int i = 0; i < t->n_ap; i++) {
            apiece *v = &t->ap[i];
            if (v->res_cnt > 0) continue;                  // someone's fetching
            if (v->idx >= ph && v->idx < hi) continue;     // hot window
            if (is_startup_crit(t, v->idx, ph, lo, fhi)) continue;
            int64_t d = v->idx < ph ? ph - v->idx : v->idx - ph;
            if (d > worst) { worst = d; victim = i; }
        }
        if (victim < 0) return NULL;   // every buffer is doing useful work
        apiece *v = &t->ap[victim];
        t->st_blocks_have -= v->have_cnt;   // partial progress discarded
        if (t->status[v->idx] == PIECE_ACTIVE) t->status[v->idx] = PIECE_NEEDED;
        slot = victim;
    }

    a = &t->ap[slot];
    if (!a->buf)  a->buf  = malloc((size_t)t->meta.piece_len);
    if (!a->have) a->have = malloc((size_t)t->blocks_per_piece);
    if (!a->own)  a->own  = malloc((size_t)t->blocks_per_piece);
    if (!a->buf || !a->have || !a->own) return NULL;  // slot stays reusable

    a->idx      = idx;
    a->nblocks  = (int)((torrent_piece_len(&t->meta, idx) + BLOCK_LEN - 1) /
                        BLOCK_LEN);
    a->have_cnt = 0;
    a->res_cnt  = 0;
    memset(a->have, 0, a->nblocks);
    memset(a->own, 0, a->nblocks);
    t->status[idx] = PIECE_ACTIVE;
    return a;
}

static bool out_has(const sess *s, int32_t piece, uint16_t block) {
    for (int i = 0; i < s->out_n; i++)
        if (s->out[i].piece == piece && s->out[i].block == block) return true;
    return false;
}

static void out_remove(sess *s, int32_t piece, uint16_t block) {
    for (int i = 0; i < s->out_n; i++)
        if (s->out[i].piece == piece && s->out[i].block == block) {
            s->out[i] = s->out[--s->out_n];
            return;
        }
}

// Hand back everything this session had on order. `mark_stalled` raises the
// racing flag on the affected pieces: used when the peer took requests and
// went quiet (the single-slow-peer deadlock v1 kept re-learning about).
static void release_out(torrentfs *t, sess *s, bool mark_stalled) {
    if (s->out_n == 0) return;
    mutexLock(&t->lock);
    for (int i = 0; i < s->out_n; i++) {
        apiece *a = ap_find(t, s->out[i].piece);
        if (a) {
            int b = s->out[i].block;
            if (b < a->nblocks && a->own[b] == (uint8_t)(s->sid + 1)) {
                a->own[b] = 0;
                a->res_cnt--;
            }
        }
        if (mark_stalled) t->piece_stalled[s->out[i].piece] = 1;
    }
    mutexUnlock(&t->lock);
    s->out_n = 0;
}

// Assign up to `want` blocks of piece `idx` to session `s`. Caller holds
// t->lock. Returns the remaining want. Reservations are exclusive except on
// stalled/startup-critical pieces, where racing is the whole point.
static int sched_piece(torrentfs *t, sess *s, int64_t idx, int want,
                       oreq *add, int *n_add, int64_t ph, int64_t lo,
                       int64_t fhi) {
    if (want <= 0) return 0;
    uint8_t st = t->status[idx];
    if (st == PIECE_DONE || st == PIECE_WRITING) return want;
    if (!bf_has_piece(s->bitfield, t->bf_len, idx)) return want;

    // hi for eviction: anything at/after ph counts as hot enough
    apiece *a = ap_get(t, idx, ph, ph + STREAM_MIN_PIECES, lo, fhi);
    if (!a) return want;

    bool dup = t->piece_stalled[idx] || is_startup_crit(t, idx, ph, lo, fhi);
    uint8_t me = (uint8_t)(s->sid + 1);
    for (int b = 0; b < a->nblocks && want > 0; b++) {
        if (a->have[b]) continue;
        if (out_has(s, (int32_t)idx, (uint16_t)b)) continue;
        if (a->own[b] == 0) {
            a->own[b] = me;
            a->res_cnt++;
        } else if (!(dup && a->own[b] != me)) {
            continue;   // reserved by someone else, and no reason to race
        }
        add[*n_add] = (oreq){ (int32_t)idx, (uint16_t)b };
        (*n_add)++;
        s->out[s->out_n++] = (oreq){ (int32_t)idx, (uint16_t)b };
        want--;
        if (s->out_n >= DEPTH_MAX) return 0;
    }
    return want;
}

// Top up a session's pipeline with the most urgent blocks its peer can serve.
static void sched_fill(torrentfs *t, sess *s) {
    if (!s->handshaked || s->choked || s->dead) return;
    // Calm mode: the deeper the player's backlog, the fewer sessions may take
    // new work. The wifi driver and bsd pay per packet ON TOP of per byte, on
    // the OS core: the whole swarm bursting at line rate every window slide
    // is what froze the console (measured: single recv() calls of 2-5 s while
    // the panel's fluid-phase speed read ~0). Blocked sessions simply stop
    // claiming; their in-flight blocks drain normally.
    if (!t->claim_allowed[s->sid]) return;
    // Fill-rate governor (when enabled): with a deep backlog, pause claiming
    // while the measured rate is above target -- this is what actually bounds
    // the bursts; a single fast peer alone saturates the wifi if allowed to.
    if (g_governor && t->calm_bps > 0 && t->rate_bps > (double)t->calm_bps)
        return;
    int want = s->depth - s->out_n;
    if (want <= 0) return;
    bool was_empty = s->out_n == 0;

    oreq add[DEPTH_MAX];
    int n_add = 0;

    mutexLock(&t->lock);
    int64_t pc = t->meta.piece_count;
    int64_t lo = t->file_first_piece, fhi = t->file_last_piece;
    if (lo < 0) lo = 0;
    if (fhi >= pc) fhi = pc - 1;
    int64_t ph = t->playhead_piece;
    if (ph < lo) ph = lo;
    if (ph > fhi) ph = fhi;
    int64_t window = STREAM_WINDOW / t->meta.piece_len;
    if (window < STREAM_MIN_PIECES) window = STREAM_MIN_PIECES;
    int64_t hi = ph + window;
    if (hi > fhi + 1) hi = fhi + 1;

    t->st_win_ph = ph; t->st_win_lo = lo; t->st_win_hi = hi;

    // Startup: the file's tail (moov atom) then head, redundantly.
    if (startup_phase(t, ph, lo)) {
        for (int64_t i = fhi; i > fhi - CRIT_TAIL && i >= lo && want > 0; i--)
            want = sched_piece(t, s, i, want, add, &n_add, ph, lo, fhi);
        for (int64_t i = lo; i < lo + CRIT_HEAD && i <= fhi && want > 0; i++)
            want = sched_piece(t, s, i, want, add, &n_add, ph, lo, fhi);
    }
    // Streaming order: lowest unfinished piece at/after the playhead first.
    for (int64_t i = ph; i < hi && want > 0; i++)
        want = sched_piece(t, s, i, want, add, &n_add, ph, lo, fhi);
    // Backfill anything missed behind the playhead (seek back, boundaries).
    for (int64_t i = lo; i < ph && want > 0; i++)
        want = sched_piece(t, s, i, want, add, &n_add, ph, lo, fhi);

    if (n_add > 0) t->st_claim_ok++; else t->st_claim_fail++;
    int act = 0;
    for (int i = 0; i < t->n_ap; i++)
        if (t->ap[i].idx >= 0) act++;
    (void)act;
    mutexUnlock(&t->lock);

    for (int i = 0; i < n_add; i++)
        queue_req(s, MSG_REQUEST, add[i].piece,
                  (uint32_t)add[i].block * BLOCK_LEN,
                  block_len_of(torrent_piece_len(&t->meta, add[i].piece),
                               add[i].block));

    // Start the stall clock when the pipeline goes from empty to armed: a peer
    // that accepts requests and never answers must not dodge the timer by
    // never delivering a first block.
    if (was_empty && s->out_n > 0) s->last_block = armGetSystemTick();
}

//-----------------------------------------------------------------------------
// Piece completion
//-----------------------------------------------------------------------------

// All blocks landed: verify from RAM and hand the buffer to the writer.
static void piece_complete(torrentfs *t, int64_t idx) {
    apiece *a = ap_find(t, idx);
    if (!a) return;
    int64_t plen = torrent_piece_len(&t->meta, idx);

    uint8_t hash[20];
    mbedtls_sha1(a->buf, (size_t)plen, hash);
    if (memcmp(hash, t->meta.piece_hashes + idx * 20, 20) != 0) {
        mutexLock(&t->lock);
        t->st_sha_fail++;
        t->st_blocks_have -= a->have_cnt;
        a->have_cnt = 0;
        a->res_cnt  = 0;
        memset(a->have, 0, a->nblocks);
        memset(a->own, 0, a->nblocks);
        snprintf(t->st_last_err, sizeof(t->st_last_err),
                 "sha fail piece %lld", (long long)idx);
        mutexUnlock(&t->lock);
        return;   // slot stays active; it will be re-fetched into the same buf
    }

    // Cancel whatever anyone still has on order for this piece (racers on a
    // stalled/critical piece): those blocks are on their way and would arrive
    // only to be thrown away — cancelled, most peers stop sending them.
    if (t->S) {
        for (int q = 0; q < MAX_SESS; q++) {
            sess *o = &t->S[q];
            if (!o->active || o->out_n == 0) continue;
            for (int i = 0; i < o->out_n;) {
                if (o->out[i].piece == (int32_t)idx) {
                    queue_req(o, MSG_CANCEL, idx,
                              (uint32_t)o->out[i].block * BLOCK_LEN,
                              block_len_of(plen, o->out[i].block));
                    o->out[i] = o->out[--o->out_n];
                } else {
                    i++;
                }
            }
        }
    }

    mutexLock(&t->lock);
    t->status[idx] = PIECE_WRITING;
    int tail = (t->wq_head + t->wq_n) % (AP_MAX + 2);
    t->wq[tail] = (wjob){ idx, plen, a->buf };
    t->wq_n++;                 // can't overflow: each job holds one ap buffer
    a->idx = -1;               // slot free; buffer ownership moved to the job
    a->buf = NULL;
    mutexUnlock(&t->lock);
}

// Writer thread: one sequential SD write + flush per finished piece, then the
// piece becomes readable (DONE) and the buffer returns to the pool. Keeping
// this off the netloop is what stops SD latency from stalling the sockets.
static void writer_main(void *arg) {
    torrentfs *t = arg;
    for (;;) {
        hb_beat(t, HB_WRITER);
        wjob j;
        bool has = false;
        mutexLock(&t->lock);
        if (t->wq_n > 0) {
            j = t->wq[t->wq_head];
            t->wq_head = (t->wq_head + 1) % (AP_MAX + 2);
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

        // Write the piece in slices, taking cache_lock per slice: with the
        // lock held across the whole 1 MB (plus whatever stall the SD card's
        // internal garbage collection adds -- whole seconds on some cards),
        // the player's read of an already-DONE piece queued behind it,
        // observed as console lag exactly when the playhead piece flips
        // INFLIGHT->DONE. Between slices any waiting reader gets in. One
        // shared fd per chunk: Horizon's FS refuses a second write-mode open
        // of the same file, so writer and readers cannot have separate ones.
        mutexLock(&t->cache_lock);
        int wci = -1;
        int64_t wco = 0;
        bool ok = piece_loc(t, j.idx, true, &wci, &wco);
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

        int nblocks = (int)((j.plen + BLOCK_LEN - 1) / BLOCK_LEN);
        mutexLock(&t->lock);
        if (ok) {
            if (t->status[j.idx] != PIECE_DONE) {
                t->status[j.idx] = PIECE_DONE;
                t->pieces_done++;
            }
            t->piece_stalled[j.idx] = 0;
            t->st_piece_ok++;
            t->st_cache_written += j.plen;
        } else {
            t->status[j.idx] = PIECE_NEEDED;   // lost: re-download
            t->st_cache_wr_fail++;
            snprintf(t->st_last_err, sizeof(t->st_last_err),
                     "cache write failed, piece %lld", (long long)j.idx);
        }
        t->st_blocks_have -= nblocks;   // no longer a RAM partial either way
        // Return the buffer to a free ap slot for reuse.
        for (int i = 0; i < t->n_ap; i++)
            if (t->ap[i].idx < 0 && !t->ap[i].buf) { t->ap[i].buf = j.buf; j.buf = NULL; break; }
        mutexUnlock(&t->lock);
        free(j.buf);   // no free slot took it (shouldn't happen); don't leak
    }
}

//-----------------------------------------------------------------------------
// Peer pool + discovery (v1 fixes, ported)
//-----------------------------------------------------------------------------

static void add_peers_cb(void *ctx, const peer_addr *peers, int n) {
    torrentfs *t = ctx;
    mutexLock(&t->lock);
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int j = 0; j < t->peer_count; j++)
            if (t->peers[j].ip == peers[i].ip && t->peers[j].port == peers[i].port) {
                dup = true;
                break;
            }
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

static bool tfs_starving(torrentfs *t) {
    mutexLock(&t->lock);
    bool s = t->st_live < STARVED_LIVE;
    mutexUnlock(&t->lock);
    return s;
}

static bool file_done(torrentfs *t) {
    mutexLock(&t->lock);
    bool d = t->pieces_done >= t->file_last_piece - t->file_first_piece + 1;
    mutexUnlock(&t->lock);
    return d;
}

static bool discovery_wait(torrentfs *t) {
    u64 freq  = t->freq;
    u64 start = armGetSystemTick();
    while (!t->stop && !file_done(t)) {
        svcSleepThread(1000000000ULL);
        u64 secs = (armGetSystemTick() - start) / freq;
        if (secs >= DISC_INTERVAL_SECS) return true;
        if (secs >= DISC_STARVED_SECS && tfs_starving(t)) return true;
    }
    return false;
}

static void announce_worker(void *arg) {
    torrentfs *t = arg;
    char e[128];
    do
        torrent_announce_cb(&t->meta, add_peers_cb, t, e, sizeof(e));
    while (discovery_wait(t));
}

static void dht_worker(void *arg) {
    torrentfs *t = arg;
    char e[128];
    do
        dht_find_peers(t->meta.info_hash, 150, 20000, add_peers_cb, t, &t->stop,
                       e, sizeof(e));
    while (discovery_wait(t));
}

//-----------------------------------------------------------------------------
// Session lifecycle
//-----------------------------------------------------------------------------

static void sess_reset(torrentfs *t, sess *s, bool conn_failed) {
    release_out(t, s, false);
    if (s->pidx >= 0) {
        u64 freq = t->freq;
        u64 backoff;
        mutexLock(&t->lock);
        t->peer_busy[s->pidx] = 0;
        if (conn_failed) {
            t->st_conn_fail++;
            if (t->peer_fails[s->pidx] < 255) t->peer_fails[s->pidx]++;
            int f = t->peer_fails[s->pidx];
            if (f > 6) f = 6;
            backoff = (u64)BACKOFF_CONN_SECS << (f - 1);
            if (backoff > BACKOFF_MAX_SECS) backoff = BACKOFF_MAX_SECS;
        } else {
            backoff = BACKOFF_DROP_SECS;
        }
        t->peer_next_try[s->pidx] = armGetSystemTick() + backoff * freq;
        mutexUnlock(&t->lock);
    }
    if (s->sock >= 0) close(s->sock);
    free(s->rx);
    free(s->tx);
    free(s->bitfield);
    free(s->adv);
    int sid = s->sid;
    memset(s, 0, sizeof(*s));
    s->sid  = sid;
    s->sock = -1;
    s->pidx = -1;
}

// Attach buffers to a raw socket and make it non-blocking. 0 ok, -1 alloc fail.
static int sess_attach(torrentfs *t, sess *s, int sock) {
    s->sock     = sock;
    s->choked   = true;
    s->depth    = DEPTH_START;
    s->tx_cap   = 4096;
    s->rx       = malloc(RX_CAP);
    s->tx       = malloc(s->tx_cap);
    s->bitfield = calloc(1, t->bf_len ? t->bf_len : 1);
    s->adv      = calloc(1, t->bf_len ? t->bf_len : 1);
    if (!s->rx || !s->tx || !s->bitfield || !s->adv) {
        free(s->rx); free(s->tx); free(s->bitfield); free(s->adv);
        s->rx = s->tx = s->bitfield = s->adv = NULL;
        s->sock = -1;
        return -1;
    }
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return 0;
}

static bool sess_open(torrentfs *t, sess *s) {
    peer_addr addr;
    int pidx = take_peer(t, &addr);
    if (pidx < 0) return false;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        mutexLock(&t->lock);
        t->peer_busy[pidx] = 0;
        t->st_sock_fail++;   // local resource (socket buffer pool), not the peer
        snprintf(t->st_last_err, sizeof(t->st_last_err), "socket(): %s",
                 strerror(errno));
        mutexUnlock(&t->lock);
        return false;
    }
    if (sess_attach(t, s, sock) != 0) {
        close(sock);
        mutexLock(&t->lock);
        t->peer_busy[pidx] = 0;
        mutexUnlock(&t->lock);
        return false;
    }

    struct sockaddr_in sa = {0};
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(addr.port);
    sa.sin_addr.s_addr = addr.ip;

    int rc = connect(sock, (struct sockaddr *)&sa, sizeof(sa));
    if (rc != 0 && errno != EINPROGRESS) {
        int e = errno;
        close(sock);
        free(s->rx); free(s->tx); free(s->bitfield); free(s->adv);
        int sid = s->sid;
        memset(s, 0, sizeof(*s));
        s->sid = sid; s->sock = -1; s->pidx = -1;
        mutexLock(&t->lock);
        t->peer_busy[pidx] = 0;
        t->st_conn_fail++;
        if (t->peer_fails[pidx] < 255) t->peer_fails[pidx]++;
        t->st_sock_fail++;
        snprintf(t->st_last_err, sizeof(t->st_last_err), "connect(): %s",
                 strerror(e));
        mutexUnlock(&t->lock);
        return false;
    }

    s->active     = true;
    s->connecting = (rc != 0);
    s->pidx       = pidx;
    s->started    = armGetSystemTick();
    s->last_rx    = s->started;
    s->last_tx    = s->started;
    return true;
}

static void sess_connected(torrentfs *t, sess *s) {
    mutexLock(&t->lock);
    t->st_conn_ok++;
    if (s->pidx >= 0) t->peer_fails[s->pidx] = 0;
    mutexUnlock(&t->lock);
    queue_handshake(s, t->meta.info_hash, t->peer_id);
}

// Advertise every DONE piece this session hasn't told its peer about yet
// (bounded per call). Reciprocation depends on peers knowing what we hold.
static void sync_haves(torrentfs *t, sess *s) {
    int sent = 0;
    int64_t pc = t->meta.piece_count;
    for (int64_t i = 0; i < pc && sent < 64; i++) {
        uint8_t bit = (uint8_t)(0x80 >> (i % 8));
        if (s->adv[i / 8] & bit) continue;
        mutexLock(&t->lock);
        bool done = t->status[i] == PIECE_DONE;
        mutexUnlock(&t->lock);
        if (!done) continue;
        s->adv[i / 8] |= bit;
        uint8_t hv[4];
        uint32_t v = htonl((uint32_t)i);
        memcpy(hv, &v, 4);
        queue_msg(s, MSG_HAVE, hv, 4);
        sent++;
    }
}

// Serve one block to the peer from the SD cache (DONE pieces only).
static void serve_block(torrentfs *t, sess *s, uint32_t index, uint32_t begin,
                        uint32_t len, uint8_t *sbuf) {
    if (len == 0 || len > BLOCK_LEN) return;
    if (index >= (uint32_t)t->meta.piece_count) return;
    if (begin + len > (uint32_t)torrent_piece_len(&t->meta, index)) return;
    mutexLock(&t->lock);
    bool done = t->status[index] == PIECE_DONE;
    mutexUnlock(&t->lock);
    if (!done) return;

    uint32_t v;
    v = htonl(index); memcpy(sbuf + 0, &v, 4);
    v = htonl(begin); memcpy(sbuf + 4, &v, 4);
    // TRY-lock: cache_lock may be held by mpv's demuxer thread mid-read, and
    // Horizon can starve that thread for seconds (see torrentfs_set_playhead)
    // -- the netloop must never wait behind it. A skipped serve just means
    // the peer re-requests the block.
    if (!mutexTryLock(&t->cache_lock)) return;
    bool ok = cache_piece_read(t, (int64_t)index, begin, sbuf + 8, len);
    mutexUnlock(&t->cache_lock);
    if (!ok) return;
    if (queue_msg(s, MSG_PIECE, sbuf, 8 + len) == 0)
        inc(t, &t->st_blocks_served);
}

static void sess_msg(torrentfs *t, sess *s, uint8_t id, uint8_t *pl,
                     uint32_t plen, uint8_t *sbuf) {
    if (id == MSG_UNCHOKE) {
        s->choked = false;
        if (!s->counted_unchoke) {
            inc(t, &t->st_unchoke_ok);
            s->counted_unchoke = true;
        }
    } else if (id == MSG_CHOKE) {
        s->choked = true;
        inc(t, &t->st_choked);
        if (s->out_n > 0) {
            // Spec-wise a choke voids our requests; hand the blocks back so
            // other sessions can take them, and re-assert interest.
            release_out(t, s, false);
            mutexLock(&t->lock);
            t->st_fetch_fail++;
            snprintf(t->st_last_err, sizeof(t->st_last_err), "choke");
            mutexUnlock(&t->lock);
        }
        queue_msg(s, MSG_INTERESTED, NULL, 0);
    } else if (id == MSG_INTERESTED) {
        inc(t, &t->st_interested_recv);
        if (!s->we_unchoked) {
            queue_msg(s, MSG_UNCHOKE, NULL, 0);
            s->we_unchoked = true;
        }
    } else if (id == MSG_HAVE) {
        if (plen == 4) {
            uint32_t hi;
            memcpy(&hi, pl, 4);
            hi = ntohl(hi);
            if (hi < (uint32_t)t->meta.piece_count)
                s->bitfield[hi / 8] |= (uint8_t)(0x80 >> (hi % 8));
        }
    } else if (id == MSG_BITFIELD) {
        if (plen == t->bf_len) {
            memcpy(s->bitfield, pl, t->bf_len);
            inc(t, &t->st_bf_ok);
        } else {
            mutexLock(&t->lock);
            t->st_bf_bad++;
            snprintf(t->st_last_err, sizeof(t->st_last_err),
                     "bitfield %u B, expected %u", (unsigned)plen,
                     (unsigned)t->bf_len);
            mutexUnlock(&t->lock);
        }
    } else if (id == MSG_REQUEST) {
        inc(t, &t->st_request_recv);
        if (s->we_unchoked && plen == 12) {
            uint32_t ri, rb, rl;
            memcpy(&ri, pl + 0, 4); ri = ntohl(ri);
            memcpy(&rb, pl + 4, 4); rb = ntohl(rb);
            memcpy(&rl, pl + 8, 4); rl = ntohl(rl);
            serve_block(t, s, ri, rb, rl, sbuf);
        }
    } else if (id == MSG_PIECE && plen >= 8) {
        uint32_t ri, rb;
        memcpy(&ri, pl + 0, 4); ri = ntohl(ri);
        memcpy(&rb, pl + 4, 4); rb = ntohl(rb);
        uint32_t blen = plen - 8;
        if (ri >= (uint32_t)t->meta.piece_count || rb % BLOCK_LEN != 0) return;
        int b = (int)(rb / BLOCK_LEN);

        bool complete = false;
        mutexLock(&t->lock);
        t->st_bytes_recv += blen;
        apiece *a = ap_find(t, (int64_t)ri);
        if (a && b < a->nblocks && !a->have[b] &&
            blen == block_len_of(torrent_piece_len(&t->meta, ri), b)) {
            memcpy(a->buf + rb, pl + 8, blen);
            a->have[b] = 1;
            a->have_cnt++;
            if (a->own[b]) { a->own[b] = 0; a->res_cnt--; }
            t->st_blocks_have++;
            complete = a->have_cnt == a->nblocks;
            s->rate_blocks++;
        }
        mutexUnlock(&t->lock);

        out_remove(s, (int32_t)ri, (uint16_t)b);
        s->last_block = armGetSystemTick();
        if (complete) piece_complete(t, (int64_t)ri);
    }
    // MSG_CANCEL / MSG_NOT_INTERESTED: nothing useful to do.
}

static void sess_service(torrentfs *t, sess *s, uint8_t *sbuf) {
    u64 lt0 = armGetSystemTick();
    ssize_t got = rx_pump(s);
    lat_add(t, LAT_RECV, lt0);
    if (got < 0) { s->dead = true; return; }
    if (got > 0) s->last_rx = armGetSystemTick();

    if (!s->handshaked) {
        int r = rx_handshake(s, t->meta.info_hash);
        if (r < 0) { s->dead = true; return; }
        if (r == 0) return;
        // Handshaked: advertise our pieces, declare interest.
        mutexLock(&t->lock);
        for (int64_t i = 0; i < t->meta.piece_count; i++)
            if (t->status[i] == PIECE_DONE)
                s->adv[i / 8] |= (uint8_t)(0x80 >> (i % 8));
        mutexUnlock(&t->lock);
        queue_msg(s, MSG_BITFIELD, s->adv, (uint32_t)t->bf_len);
        queue_msg(s, MSG_INTERESTED, NULL, 0);
        s->last_adv = armGetSystemTick();
    }

    uint8_t id;
    uint8_t *payload;
    uint32_t plen;
    int r;
    while ((r = rx_next(s, &id, &payload, &plen)) == 1) {
        sess_msg(t, s, id, payload, plen, sbuf);
        if (s->dead) return;
    }
    if (r < 0) { s->dead = true; return; }

    // Refill at half-empty, not on every message: sched_fill takes the global
    // lock and walks the window, so batching the refills keeps it off the
    // per-block hot path while still never letting the pipeline drain.
    if (s->out_n < s->depth / 2) sched_fill(t, s);
}

//-----------------------------------------------------------------------------
// Acceptors + net loop
//-----------------------------------------------------------------------------

static void acceptor_main(void *arg) {
    torrentfs *t = arg;
    while (!t->stop) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int fd = accept(t->listen_sock, (struct sockaddr *)&ca, &cl);
        if (fd < 0) continue;  // 1 s SO_RCVTIMEO fired; re-check stop
        if (t->stop) { close(fd); break; }
        mutexLock(&t->lock);
        bool queued = false;
        for (int i = 0; i < INQ; i++)
            if (t->incoming[i] < 0) { t->incoming[i] = fd; queued = true; break; }
        if (queued) t->st_incoming++;
        mutexUnlock(&t->lock);
        if (!queued) close(fd);
    }
}

static void netloop_main(void *arg) {
    torrentfs *t = arg;

    sess *S            = calloc(MAX_SESS, sizeof(sess));
    struct pollfd *pfd = calloc(MAX_SESS, sizeof(struct pollfd));
    int *map           = calloc(MAX_SESS, sizeof(int));
    uint8_t *sbuf      = malloc(8 + BLOCK_LEN);
    if (!S || !pfd || !map || !sbuf) goto out;

    for (int i = 0; i < MAX_SESS; i++) { S[i].sid = i; S[i].sock = -1; S[i].pidx = -1; }
    t->S = S;

    u64 freq = t->freq;

    while (!t->stop && !file_done(t)) {
        hb_beat(t, HB_NET);
        // Adopt incoming peers first: they are reachable by definition and on
        // a NAT'd swarm often the only seeds that can talk to us at all.
        for (int i = 0; i < MAX_SESS; i++) {
            if (S[i].active) continue;
            int fd = -1;
            mutexLock(&t->lock);
            for (int q = 0; q < INQ; q++)
                if (t->incoming[q] >= 0) { fd = t->incoming[q]; t->incoming[q] = -1; break; }
            mutexUnlock(&t->lock);
            if (fd < 0) break;
            if (sess_attach(t, &S[i], fd) != 0) { close(fd); continue; }
            S[i].active  = true;
            S[i].pidx    = -1;
            S[i].started = S[i].last_rx = S[i].last_tx = armGetSystemTick();
            sess_connected(t, &S[i]);
        }

        // Outbound attempts, bounded so pending connects never crowd out the
        // live sessions, and stopped entirely while enough sessions are live
        // (DIAL_STOP_LIVE) -- resumed the moment attrition drops us back
        // under, so a scarce swarm keeps hunting.
        int connecting = 0, live_now = 0;
        for (int i = 0; i < MAX_SESS; i++) {
            if (!S[i].active) continue;
            if (S[i].connecting) connecting++;
            else if (S[i].handshaked) live_now++;
        }
        for (int i = 0; i < MAX_SESS && connecting < MAX_CONNECTING &&
                        live_now < DIAL_STOP_LIVE; i++) {
            if (S[i].active) continue;
            if (!sess_open(t, &S[i])) break;
            if (S[i].connecting) connecting++;
        }

        int n = 0;
        for (int i = 0; i < MAX_SESS; i++) {
            if (!S[i].active) continue;
            pfd[n].fd     = S[i].sock;
            pfd[n].events = POLLIN;
            if (S[i].connecting || tx_pending(&S[i]))
                pfd[n].events |= POLLOUT;
            pfd[n].revents = 0;
            map[n] = i;
            n++;
        }
        if (n == 0) {
            svcSleepThread(200000000ULL);  // no peers yet; wait for discovery
            continue;
        }

        u64 lt0 = armGetSystemTick();
        int rc = poll(pfd, (nfds_t)n, 200);
        lat_add(t, LAT_POLL, lt0);  // idle poll tops at ~200 ms; more = bsd stalled
        if (rc < 0) { svcSleepThread(50000000ULL); continue; }

        // Coalesce wakeups: poll() returns on the FIRST pending packet, so
        // servicing right away makes this loop run once per packet -- at
        // streaming rates that is thousands of poll+recv IPCs per second
        // against the bsd sysmodule, all serviced on the OS core (core 3),
        // enough on its own to stutter the whole console. Napping a few ms
        // first lets more packets land in the kernel-side socket buffers
        // (32..256 KB each -- ample for 4 ms at line rate) so each pass
        // drains a big batch instead. The loop keeps every guarantee: poll is
        // level-triggered, so anything arriving during the nap is picked up
        // by the next poll, and 4 ms is noise against swarm RTTs -- the
        // request pipeline is depth-managed (DEPTH_*), not turnaround-managed.
        if (rc > 0) svcSleepThread(4000000ULL);  // 4 ms

        for (int k = 0; k < n && !t->stop; k++) {
            sess *s = &S[map[k]];
            short rev = pfd[k].revents;
            if (!s->active) continue;
            if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
                sess_reset(t, s, s->connecting);
                continue;
            }
            if (s->connecting) {
                if (!(rev & POLLOUT)) continue;
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                if (getsockopt(s->sock, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 ||
                    soerr != 0) {
                    sess_reset(t, s, true);
                    continue;
                }
                s->connecting = false;
                sess_connected(t, s);
            }
            if (rev & POLLOUT) {
                u64 lt1 = armGetSystemTick();
                int fr  = tx_flush(s);
                lat_add(t, LAT_SEND, lt1);
                if (fr != 0) { sess_reset(t, s, false); continue; }
            }
            if (rev & POLLIN) {
                sess_service(t, s, sbuf);
                if (s->dead) { sess_reset(t, s, false); continue; }
            }
        }

        // Upkeep: timers, keep-alives, HAVE sync, rate/depth, idle refills.
        u64 now = armGetSystemTick();

        // Calm mode, graduated: session budget by backlog depth. A flat
        // 2-session trickle drained to the floor and then woke the WHOLE
        // swarm at once -- a 65%→100%→65% seesaw on the OS core. A ramp
        // adds capacity a few sessions at a time, so throughput tracks the
        // playback rate instead of oscillating between extremes. A torrent
        // that can barely keep up stays under 10 s and is never narrowed.
        {
            int bl = t->backlog_ms;
            int budget = MAX_SESS;           // < 10 s: full swarm
            // Floor of 3: with a single allowed session, one peer hiccup
            // starves the playhead piece at the read frontier and the whole
            // player chains up behind the blocked read (a logged ~3 s freeze
            // with wifi=3 and every syscall probe at 0). Three keep the
            // critical path redundant while staying calm.
            if      (bl >= 25000) budget = 3;
            else if (bl >= 15000) budget = 5;
            else if (bl >= 10000) budget = 8;
            t->calm_now = budget;

            // Rate target for the fill governor (0 = unlimited). Under 10 s
            // of backlog nothing is capped -- a stream that struggles for
            // 1080p lives entirely down there and keeps its full speed.
            if      (bl < 10000) t->calm_bps = 0;
            else if (bl < 15000) t->calm_bps = 4 << 20;   // 4 MB/s
            else if (bl < 20000) t->calm_bps = 3 << 20;
            else if (bl < 25000) t->calm_bps = 2 << 20;
            else                 t->calm_bps = 3 << 19;   // 1.5 MB/s

            // Download-rate EWMA, netloop-local (st_bytes_recv is also only
            // written on this thread), refreshed about once a second.
            if (t->rate_last_tick == 0) {
                t->rate_last_tick  = now;
                t->rate_last_bytes = t->st_bytes_recv;
            } else if (now - t->rate_last_tick >= freq) {
                double secs = (double)(now - t->rate_last_tick) / (double)freq;
                double inst = (double)(t->st_bytes_recv - t->rate_last_bytes) / secs;
                t->rate_bps        = t->rate_bps * 0.5 + inst * 0.5;
                t->rate_last_bytes = t->st_bytes_recv;
                t->rate_last_tick  = now;
            }

            if (budget >= MAX_SESS) {
                memset(t->claim_allowed, 1, MAX_SESS);
            } else {
                // Allow the `budget` fastest handshaked+unchoked sessions.
                memset(t->claim_allowed, 0, MAX_SESS);
                for (int k = 0; k < budget; k++) {
                    int best = -1;
                    double br = -1.0;
                    for (int i = 0; i < MAX_SESS; i++) {
                        sess *s = &S[i];
                        if (!s->active || !s->handshaked || s->choked) continue;
                        if (t->claim_allowed[i]) continue;
                        if (s->rate_ewma > br) { br = s->rate_ewma; best = i; }
                    }
                    if (best < 0) break;
                    t->claim_allowed[best] = 1;
                }
            }
        }
        int live = 0, claiming = 0, idle_unchoked = 0, bf_empty = 0;
        for (int i = 0; i < MAX_SESS; i++) {
            sess *s = &S[i];
            if (!s->active) continue;

            if (s->connecting) {
                if (now - s->started >= freq * CONNECT_SECS) {
                    inc(t, &t->st_conn_timeout);
                    sess_reset(t, s, true);
                }
                continue;
            }
            if (!s->handshaked && now - s->started >= freq * PREHS_SECS) {
                sess_reset(t, s, false);
                continue;
            }
            if (s->handshaked && now - s->last_rx >= freq * IDLE_SECS) {
                sess_reset(t, s, false);
                continue;
            }

            if (s->handshaked) {
                live++;
                if (s->out_n > 0) claiming++;
                else if (!s->choked) idle_unchoked++;
                bool any = false;
                for (size_t b = 0; b < t->bf_len && !any; b++)
                    if (s->bitfield[b]) any = true;
                if (!any) bf_empty++;

                // Keep the peer aware we're alive even when idle/choked;
                // without this WE look like the silent one and get dropped.
                if (now - s->last_tx >= freq * KEEPALIVE_SECS) {
                    uint8_t ka[4] = {0, 0, 0, 0};
                    tx_append(s, ka, 4);
                }
                if (now - s->last_adv >= freq) {
                    sync_haves(t, s);
                    s->last_adv = now;
                }

                // A peer sitting on reservations without delivering strands
                // those blocks for everyone. Free them and let peers race.
                if (s->out_n > 0 && s->last_block &&
                    now - s->last_block >= freq * STALL_SECS) {
                    release_out(t, s, true);
                    mutexLock(&t->lock);
                    t->st_fetch_fail++;
                    snprintf(t->st_last_err, sizeof(t->st_last_err), "stall");
                    mutexUnlock(&t->lock);
                }

                // Per-second: rate EWMA -> pipeline depth (~1.5 s of rate).
                if (s->last_rate == 0) s->last_rate = now;
                if (now - s->last_rate >= freq) {
                    double secs = (double)(now - s->last_rate) / (double)freq;
                    double inst = (double)s->rate_blocks / secs;
                    s->rate_ewma = s->rate_ewma * 0.6 + inst * 0.4;
                    s->rate_blocks = 0;
                    s->last_rate = now;
                    int d = (int)(s->rate_ewma * 1.5);
                    if (d < DEPTH_MIN) d = DEPTH_MIN;
                    if (d > DEPTH_MAX) d = DEPTH_MAX;
                    s->depth = d;
                }

                // Refill here too, NOT only on POLLIN: an unchoked peer we
                // never ask anything of has nothing to send us, so waiting
                // for its traffic to re-schedule it idles it forever (v1's
                // decay-to-zero bug).
                if (!s->choked && s->out_n < s->depth / 2)
                    sched_fill(t, s);
            }

            if (tx_pending(s)) {
                u64 lt1 = armGetSystemTick();
                int fr  = tx_flush(s);
                lat_add(t, LAT_SEND, lt1);
                if (fr != 0) sess_reset(t, s, false);
            }
        }

        int pending = 0;
        for (int i = 0; i < MAX_SESS; i++)
            if (S[i].active && S[i].connecting) pending++;
        mutexLock(&t->lock);
        t->st_live          = live;
        t->st_claiming      = claiming;
        t->st_idle_unchoked = idle_unchoked;
        t->st_bf_empty      = bf_empty;
        t->st_connecting    = pending;
        if (live > t->st_peak_live) t->st_peak_live = live;
        mutexUnlock(&t->lock);
    }

out:
    if (S) {
        for (int i = 0; i < MAX_SESS; i++)
            if (S[i].active) sess_reset(t, &S[i], false);
    }
    t->S = NULL;
    free(S);
    free(pfd);
    free(map);
    free(sbuf);
}

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------

torrentfs *torrentfs_open(const char *source, const char *cache_path,
                          char *err, size_t errlen) {
    return torrentfs_open_file(source, cache_path, -1, err, errlen);
}

torrentfs *torrentfs_open_file(const char *source, const char *cache_path,
                               int file_index, char *err, size_t errlen) {
    torrentfs *t = calloc(1, sizeof(*t));
    if (!t) { snprintf(err, errlen, "out of memory"); return NULL; }

    for (int i = 0; i < INQ; i++) t->incoming[i] = -1;
    for (int i = 0; i < AP_MAX; i++) t->ap[i].idx = -1;
    t->listen_sock = -1;
    t->freq = armGetSystemTickFreq();

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

    t->blocks_per_piece =
        (int)((t->meta.piece_len + BLOCK_LEN - 1) / BLOCK_LEN);
    t->bf_len = (size_t)((t->meta.piece_count + 7) / 8);
    t->status        = calloc(1, (size_t)t->meta.piece_count);
    t->piece_stalled = calloc(1, (size_t)t->meta.piece_count);

    int64_t n_ap = RAM_BUDGET / t->meta.piece_len;
    if (n_ap < AP_MIN) n_ap = AP_MIN;
    if (n_ap > AP_MAX) n_ap = AP_MAX;
    t->n_ap = (int)n_ap;

    snprintf(t->cache_base, sizeof(t->cache_base), "%s", cache_path);
    mutexInit(&t->cache_lock);
    mutexInit(&t->lock);
    for (int i = 0; i < CACHE_MAX_CHUNKS; i++) t->chunks[i] = -1;
    memset(t->claim_allowed, 1, MAX_SESS);  // unrestricted until first upkeep
    t->calm_now = MAX_SESS;
    t->slots_per_chunk = CACHE_CHUNK / t->meta.piece_len;  // >= 64: pieces cap at 16 MB
    t->piece_slot = malloc((size_t)t->meta.piece_count * sizeof(int32_t));
    if (t->piece_slot)
        for (int64_t i = 0; i < t->meta.piece_count; i++) t->piece_slot[i] = -1;
    cache_delete_all(t);  // a previous run's leftovers (crash) are dead weight
    if (!t->status || !t->piece_stalled || !t->piece_slot ||
        t->slots_per_chunk < 1 || cache_chunk(t, 0) < 0) {
        snprintf(err, errlen, "cache/status alloc failed");
        torrentfs_close(t);
        return NULL;
    }

    memcpy(t->peer_id, "-SW0002-", 8);
    srand((unsigned)time(NULL));
    for (int i = 8; i < 20; i++) t->peer_id[i] = (uint8_t)(rand() % 256);

    if (seed_count > 0) add_peers_cb(t, seed_peers, seed_count);

    if (threadCreate(&t->announce_thread, announce_worker, t, NULL,
                     0x20000, 0x2C, -2) == 0) {
        t->announce_started = true;
        threadStart(&t->announce_thread);
    }
    if (threadCreate(&t->dht_thread, dht_worker, t, NULL, 0x20000, 0x2C, -2) == 0) {
        t->dht_started = true;
        threadStart(&t->dht_thread);
    }

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls >= 0) {
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        struct sockaddr_in la = {0};
        la.sin_family      = AF_INET;
        la.sin_addr.s_addr = INADDR_ANY;
        la.sin_port        = htons(LISTEN_PORT);
        if (bind(ls, (struct sockaddr *)&la, sizeof(la)) == 0 && listen(ls, 16) == 0)
            t->listen_sock = ls;
        else
            close(ls);
    }
    if (t->listen_sock >= 0) {
        for (int i = 0; i < ACCEPTORS; i++) {
            if (threadCreate(&t->acceptors[i], acceptor_main, t, NULL,
                             0x20000, 0x2C, -2) != 0) {
                t->acceptors[i].handle = 0;
                continue;
            }
            threadStart(&t->acceptors[i]);
            t->n_acceptors++;
        }
    }

    if (threadCreate(&t->writer, writer_main, t, NULL, 0x20000, 0x2C, -2) == 0) {
        t->writer_started = true;
        threadStart(&t->writer);
    } else {
        snprintf(err, errlen, "could not start the writer thread");
        torrentfs_close(t);
        return NULL;
    }

    // Priority 0x2B, one notch ABOVE the app default (0x2C): Horizon never
    // preempts between equal priorities, so when mpv's decode threads go
    // CPU-bound (A/V-desync catch-up) every one of this loop's ~100 IPC
    // yield points waited a full burst for the core back -- measured as one
    // iteration stretching to 15 s (hb net=14635 while wr/ui/rd stayed
    // fresh). One notch higher, it preempts its way back in; its own CPU
    // share is a few percent, so the decoder barely notices.
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
    // shutdown() (not close()) wakes acceptors without invalidating the fd out
    // from under a concurrent accept() — see v1's data-abort note.
    if (tfs->listen_sock >= 0)
        shutdown(tfs->listen_sock, SHUT_RDWR);
    for (int i = 0; i < ACCEPTORS; i++) {
        if (tfs->acceptors[i].handle == 0) continue;
        threadWaitForExit(&tfs->acceptors[i]);
        threadClose(&tfs->acceptors[i]);
        tfs->acceptors[i].handle = 0;
    }
    if (tfs->listen_sock >= 0) {
        close(tfs->listen_sock);
        tfs->listen_sock = -1;
    }
    if (tfs->announce_started) {
        threadWaitForExit(&tfs->announce_thread);
        threadClose(&tfs->announce_thread);
        tfs->announce_started = false;
    }
    if (tfs->dht_started) {
        threadWaitForExit(&tfs->dht_thread);
        threadClose(&tfs->dht_thread);
        tfs->dht_started = false;
    }
    if (tfs->netloop_started) {
        threadWaitForExit(&tfs->netloop);
        threadClose(&tfs->netloop);
        tfs->netloop_started = false;
    }
    // The writer drains its queue after stop, saving finished pieces. It must
    // be joined after the netloop (which can still push jobs).
    if (tfs->writer_started) {
        threadWaitForExit(&tfs->writer);
        threadClose(&tfs->writer);
        tfs->writer_started = false;
    }
    for (int i = 0; i < INQ; i++)
        if (tfs->incoming[i] >= 0) { close(tfs->incoming[i]); tfs->incoming[i] = -1; }
    for (int i = 0; i < CACHE_MAX_CHUNKS; i++)
        if (tfs->chunks[i] >= 0) { close(tfs->chunks[i]); tfs->chunks[i] = -1; }
    cache_delete_all(tfs);  // the cache is per-playback scratch, not a library
    for (int i = 0; i < AP_MAX; i++) {
        free(tfs->ap[i].buf);
        free(tfs->ap[i].have);
        free(tfs->ap[i].own);
    }
    free(tfs->status);
    free(tfs->piece_stalled);
    free(tfs->piece_slot);
    torrent_unload(&tfs->meta);
    free(tfs);
}

int64_t torrentfs_size(const torrentfs *tfs) {
    return tfs->stream_size;
}

// LOCK-FREE ON PURPOSE, like have_piece below: these run on mpv's demuxer
// thread. Horizon does not timeslice equal-priority threads, so a CPU-bound
// mpv decode thread can starve the demuxer's core for whole seconds -- and
// when the demuxer held t->lock at that moment, the netloop (which takes
// t->lock constantly) froze with it. Measured: hb rd=6247ms while net=4031ms
// starved and the writer on the same core ran free. No mpv-driven call may
// wait on -- or hold -- an engine lock. An aligned 64-bit store is atomic on
// AArch64; readers see either the old or the new playhead, both fine.
void torrentfs_set_playhead(torrentfs *tfs, int64_t offset) {
    int64_t abs = tfs->stream_offset + offset;
    ((torrentfs *)tfs)->playhead_piece = abs / tfs->meta.piece_len;
}

static bool have_piece(torrentfs *t, int64_t idx) {
    // Lock-free byte read (see torrentfs_set_playhead). DONE is terminal and
    // only set once the piece is fully on disk, so the worst a stale read
    // costs is one extra 20 ms wait.
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

    // Racy increment on purpose: diagnostic counter, and this is the mpv
    // demuxer thread -- it must not touch t->lock (see torrentfs_set_playhead).
    if ((int64_t)got < can_read) ((torrentfs *)tfs)->st_cache_rd_short++;
    // Short reads are legal for mpv; an error would kill the stream for good.
    return (int64_t)got;
}

void torrentfs_cancel(torrentfs *tfs) {
    tfs->stop = true;
}

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
    mutexLock((Mutex *)&tfs->lock);
    if (live) *live = tfs->st_live;
    if (peak) *peak = tfs->st_peak_live;
    if (connecting) *connecting = tfs->st_connecting;
    mutexUnlock((Mutex *)&tfs->lock);
}

void torrentfs_claim_stats(const torrentfs *tfs, int *claiming, int *idle) {
    mutexLock((Mutex *)&tfs->lock);
    if (claiming) *claiming = tfs->st_claiming;
    if (idle) *idle = tfs->st_idle_unchoked;
    mutexUnlock((Mutex *)&tfs->lock);
}

void torrentfs_piece_debug(const torrentfs *tfs, int64_t idx, int *status,
                           int *have, int *req, int *total) {
    torrentfs *t = (torrentfs *)tfs;
    int h = 0, r = 0, nb = 0, st = -1;
    mutexLock(&t->lock);
    if (idx >= 0 && idx < t->meta.piece_count) {
        st = t->status[idx];
        nb = (int)((torrent_piece_len(&t->meta, idx) + BLOCK_LEN - 1) / BLOCK_LEN);
        apiece *a = ap_find(t, idx);
        if (a) {
            h = a->have_cnt;
            r = a->res_cnt;
        } else if (st == PIECE_DONE || st == PIECE_WRITING) {
            h = nb;
        }
    }
    mutexUnlock(&t->lock);
    if (status) *status = st;
    if (have) *have = h;
    if (req) *req = r;
    if (total) *total = nb;
}

int64_t torrentfs_stored_bytes(const torrentfs *tfs) {
    mutexLock((Mutex *)&tfs->lock);
    int64_t b = tfs->pieces_done * tfs->meta.piece_len +
                tfs->st_blocks_have * (int64_t)BLOCK_LEN;
    mutexUnlock((Mutex *)&tfs->lock);
    return b;  // approximate: a piece's last block may be short
}

void torrentfs_cache_stats(const torrentfs *tfs, int *wr_fail, int *rd_short,
                           int64_t *total_bytes) {
    mutexLock((Mutex *)&tfs->lock);
    if (wr_fail) *wr_fail = tfs->st_cache_wr_fail;
    if (rd_short) *rd_short = tfs->st_cache_rd_short;
    mutexUnlock((Mutex *)&tfs->lock);
    if (total_bytes) *total_bytes = tfs->meta.piece_count * tfs->meta.piece_len;
}

int64_t torrentfs_cache_written(const torrentfs *tfs) {
    mutexLock((Mutex *)&tfs->lock);
    int64_t b = tfs->st_cache_written;
    mutexUnlock((Mutex *)&tfs->lock);
    return b;
}

void torrentfs_set_backlog(torrentfs *tfs, int ms) {
    tfs->backlog_ms = ms;
}

int torrentfs_calm(const torrentfs *tfs) {
    return tfs->calm_now;  // sessions allowed to claim; MAX_SESS = unrestricted
}

void torrentfs_hb_ui(torrentfs *tfs) {
    hb_beat(tfs, HB_UI);
}

void torrentfs_heartbeats(const torrentfs *tfs, uint32_t age_ms[4],
                          int core[4]) {
    u64 now  = armGetSystemTick();
    u64 freq = tfs->freq ? tfs->freq : 1;
    for (int i = 0; i < 4; i++) {
        u64 tk    = tfs->hb_tick[i];
        age_ms[i] = tk ? (uint32_t)((now - tk) * 1000 / freq) : 0;
        core[i]   = tfs->hb_core[i];
    }
}

void torrentfs_lat_stats(const torrentfs *tfs, uint32_t count[5],
                         uint64_t max_us[5]) {
    torrentfs *t = (torrentfs *)tfs;
    u64 freq = t->freq ? t->freq : 1;
    for (int i = 0; i < 5; i++) {
        count[i]  = t->lat_n[i];
        max_us[i] = t->lat_max[i] * 1000000ULL / freq;
        t->lat_max[i] = 0;  // peaks are per read interval
    }
}

void torrentfs_claim_debug(const torrentfs *tfs, int64_t *ph, int64_t *lo,
                           int64_t *hi, int *fail, int *ok, int *inflight) {
    torrentfs *t = (torrentfs *)tfs;
    mutexLock(&t->lock);
    if (ph) *ph = t->st_win_ph;
    if (lo) *lo = t->st_win_lo;
    if (hi) *hi = t->st_win_hi;
    if (fail) *fail = t->st_claim_fail;
    if (ok) *ok = t->st_claim_ok;
    if (inflight) {
        int act = 0;
        for (int i = 0; i < t->n_ap; i++)
            if (t->ap[i].idx >= 0) act++;
        *inflight = act;
    }
    mutexUnlock(&t->lock);
}

void torrentfs_bitfield_stats(const torrentfs *tfs, int *empty, int *ok, int *bad) {
    mutexLock((Mutex *)&tfs->lock);
    if (empty) *empty = tfs->st_bf_empty;
    if (ok) *ok = tfs->st_bf_ok;
    if (bad) *bad = tfs->st_bf_bad;
    mutexUnlock((Mutex *)&tfs->lock);
}

void torrentfs_fail_kinds(const torrentfs *tfs, int *sock_fail, int *timeouts) {
    mutexLock((Mutex *)&tfs->lock);
    if (sock_fail) *sock_fail = tfs->st_sock_fail;
    if (timeouts) *timeouts = tfs->st_conn_timeout;
    mutexUnlock((Mutex *)&tfs->lock);
}

void torrentfs_debug_counts(const torrentfs *tfs, int out[10]) {
    mutexLock((Mutex *)&tfs->lock);
    out[0] = tfs->st_conn_ok;
    out[1] = tfs->st_conn_fail;
    out[2] = tfs->st_unchoke_ok;
    out[3] = tfs->st_choked;
    out[4] = tfs->st_piece_ok;
    out[5] = tfs->st_fetch_fail;
    out[6] = tfs->st_sha_fail;
    out[7] = tfs->st_blocks_served;
    out[8] = tfs->st_interested_recv;
    out[9] = tfs->st_request_recv;
    mutexUnlock((Mutex *)&tfs->lock);
}

int64_t torrentfs_piece_len(const torrentfs *tfs) {
    return tfs->meta.piece_len;
}

const char *torrentfs_name(const torrentfs *tfs) {
    return tfs->meta.name;
}

int torrentfs_piece_done(const torrentfs *tfs, int64_t idx) {
    if (idx < 0 || idx >= tfs->meta.piece_count) return 0;
    mutexLock((Mutex *)&tfs->lock);
    int d = tfs->status[idx] == PIECE_DONE;
    mutexUnlock((Mutex *)&tfs->lock);
    return d;
}

int torrentfs_incoming_count(const torrentfs *tfs) {
    mutexLock((Mutex *)&tfs->lock);
    int n = tfs->st_incoming;
    mutexUnlock((Mutex *)&tfs->lock);
    return n;
}

int64_t torrentfs_bytes_recv(const torrentfs *tfs) {
    mutexLock((Mutex *)&tfs->lock);
    int64_t b = tfs->st_bytes_recv;
    mutexUnlock((Mutex *)&tfs->lock);
    return b;
}

void torrentfs_last_err(const torrentfs *tfs, char *buf, size_t len) {
    mutexLock((Mutex *)&tfs->lock);
    snprintf(buf, len, "%s", tfs->st_last_err);
    mutexUnlock((Mutex *)&tfs->lock);
}
