#include "torrentfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <switch.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <poll.h>
#include <errno.h>
#include <unistd.h>
#include <mbedtls/sha1.h>

#include "torrent.h"
#include "peer.h"
#include "dhtclient.h"

// Budget against the 16-session BSD pool: every *blocking* socket call holds one
// session for its whole duration. The peer sessions no longer cost one each --
// they all share a single poll() (see netloop_main) -- so the standing cost is
// just: 1 netloop + 2 acceptors parked in accept() + DHT + µTP = 5.
#define INCOMING_ACCEPTORS 2
// Accepted-but-not-yet-adopted sockets handed from the acceptors to the loop.
#define INCOMING_QUEUE     32
#define PEER_LISTEN_PORT   6881  // must match the port announced to trackers (udp_tracker.c)
#define TFS_MAX_PEERS 256
// Block requests a session keeps in flight. This is a *reservation* budget, not
// just a pipelining depth: whatever a session holds, nobody else may fetch, so
// it has to suit both ends of the range we see.
//
//  - Too deep (it was 64): on a big torrent with ~80 sessions that reserved
//    4096 blocks against a window holding ~3300, so the window was permanently
//    fully reserved and a slow peer could sit on 64 blocks of the very piece
//    the player was waiting for.
//  - Too shallow (it was 8): on a small torrent the pieces are small too (a
//    0.33 GB torrent has ~21 blocks per piece), so 2-3 sessions filled a piece
//    and the other peers found nothing to do -- with only ~20 peers that starves
//    the download outright.
//
// 24 keeps ~80 sessions under the big-torrent window (1920 < 3300) while still
// giving a small swarm's peers enough in flight to stay busy.
#define SESS_PIPELINE 24

// Streaming-critical pieces: the first CRIT_HEAD and last CRIT_TAIL pieces. An
// MP4 moov atom (which the demuxer must read before it can start, and which mpv
// seeks to the end of the file to find) usually lives at one end. Fetching both
// ends before the streaming body is what lets playback start; otherwise the
// player blocks on a tail seek while every worker downloads the head.
#define CRIT_HEAD 2
#define CRIT_TAIL 3

// How far ahead of the playhead workers are allowed to download. Bounds the
// read-ahead buffer so fast workers stay focused just ahead of what the player
// is reading instead of racing to the end of the file (which starves the piece
// the player is blocked on). Kept at/under mpv's demuxer-max-bytes (64 MB).
#define STREAM_WINDOW (32 * 1024 * 1024)

// ...but never fewer than this many pieces, whatever the piece size.
//
// This is a streaming trade-off, not a throughput one. Too narrow (it was 8)
// and most sessions find nothing to claim and idle. Too wide (it was 96) and
// the opposite happens: dozens of peers each advance a *different* piece, so
// every piece crawls at 1/Nth speed while the player sits blocked on the one
// piece it needs next -- 177 MB fetched, 22 pieces finished, zero buffer.
// Keep it just wide enough to feed a healthy number of sessions; the surplus
// peers pile onto the lowest unfinished pieces via the rescue paths in
// claim_piece, which is exactly where the player is waiting.
#define STREAM_MIN_PIECES 16


// Cache chunking. FAT32 (what most Switch SD cards use) refuses to grow a file
// past 4 GB, so the cache is split into files of CACHE_CHUNK bytes each. 1 GB
// keeps a wide margin under the limit and keeps the file count small.
#define CACHE_CHUNK      (1LL << 30)
#define CACHE_MAX_CHUNKS 64            // 64 GB of torrent, far past anything real

// PIECE_VERIFYING: all blocks are on disk and one session is hashing it. It
// keeps the other sessions sharing that piece from each re-reading and re-
// hashing the same 4 MB off the SD card.
enum { PIECE_NEEDED = 0, PIECE_INFLIGHT = 1, PIECE_DONE = 2, PIECE_VERIFYING = 3 };

struct torrentfs {
    torrent_meta meta;

    // The single file we expose to mpv (largest file in the torrent). Reads are
    // relative to this file; we translate to absolute torrent offsets.
    int64_t stream_offset;   // absolute start of the target file
    int64_t stream_size;     // target file length
    int64_t file_first_piece; // first piece overlapping the target file
    int64_t file_last_piece;  // last piece overlapping the target file

    // Downloaded pieces, cached on the SD card. Split across several files:
    // Switch SD cards are usually FAT32, which caps a single file at 4 GB, and a
    // 1080p movie torrent goes well past that. As one file, every write beyond
    // 4 GB silently failed -- including the tail pieces holding an MP4's moov
    // atom, which the player must read before it can start. The header could
    // therefore never arrive no matter how much was downloaded.
    FILE *chunks[CACHE_MAX_CHUNKS];
    char cache_base[192];
    Mutex cache_lock;

    Mutex lock;           // guards status[], playhead, stop, peer cursor
    uint8_t *status;      // one byte per piece
    uint8_t *block_have;  // bitmap: one bit per block of the whole torrent
    // One bit per block, set while some session has an outstanding request for
    // it. Without this, every session piling onto the piece the player is
    // waiting for asks for the *same* blocks -- 61 sessions were sharing 10
    // pieces, so the same bytes arrived ~6 times and throughput collapsed as
    // peers were added. Claiming at block granularity lets them split the work.
    uint8_t *block_req;
    // One byte per piece, raised when a session's reservations on it expired
    // without delivering. Only these pieces are fetched redundantly: it is the
    // difference between "this piece is stuck" and "this piece matters", and
    // duplicating on the latter had 45 sessions re-fetching one piece (79% of
    // traffic wasted) while duplicating on neither froze playback outright.
    uint8_t *piece_stalled;
    int blocks_per_piece; // blocks in a full piece (bpp), for block indexing
    int64_t pieces_done;
    int64_t playhead_piece;
    bool stop;

    peer_addr peers[TFS_MAX_PEERS];
    uint8_t peer_fails[TFS_MAX_PEERS];  // consecutive connect failures per peer
    uint8_t peer_busy[TFS_MAX_PEERS];   // 1 while a session holds this peer
    u64 peer_next_try[TFS_MAX_PEERS];   // tick before which we leave the peer alone
    int peer_count;
    int next_peer;

    uint8_t peer_id[20];

    int listen_sock;                       // TCP listen socket, or -1 if disabled
    Thread acceptors[INCOMING_ACCEPTORS];  // accept incoming peers, hand to loop
    int n_acceptors;
    int incoming[INCOMING_QUEUE];          // accepted fds waiting for the loop

    Thread netloop;         // the single poll()-driven peer session thread
    bool netloop_started;

    Thread dht_thread;
    bool dht_started;
    bool dht_running;   // true while the DHT lookup may still add peers

    Thread announce_thread;
    bool announce_started;
    bool announce_running;  // true while trackers may still add peers

    // Rough worker diagnostics (guarded by lock).
    int st_conn_ok, st_conn_fail, st_unchoke_ok, st_choked;
    int st_piece_ok, st_fetch_fail, st_sha_fail;
    int st_blocks_served;    // blocks we uploaded to peers
    int st_incoming;         // peers that connected to us (incoming)
    int64_t st_bytes_recv;   // total payload bytes received (incl. partial pieces)
    int st_interested_recv;  // times a peer told us it is interested in our pieces
    int st_request_recv;     // block requests peers sent us
    int st_sock_fail;        // socket()/connect() refused outright (resources)
    int st_conn_timeout;     // SYN sent, peer never answered (dead/NAT'd peer)
    int st_connecting;       // slots sitting in a pending outbound connect
    int st_claiming;         // live sessions actually downloading a piece
    int st_idle_unchoked;    // unchoked sessions with nothing claimed (starved)
    int st_bf_empty;         // handshaked sessions whose bitfield is still empty
    int st_bf_ok;            // bitfield messages accepted
    int st_bf_bad;           // bitfield messages dropped on a length mismatch
    // Last window claim_piece actually used, plus how often it came back empty.
    // With ~1200 pieces outstanding and a rescue path that takes any non-DONE
    // piece, an empty result should be impossible -- so if it happens, these say
    // which range it was really searching.
    int64_t st_win_ph, st_win_lo, st_win_hi;
    int st_claim_fail, st_claim_ok;
    int st_inflight;         // pieces currently marked PIECE_INFLIGHT
    // Cache I/O that silently didn't happen. The whole torrent is one file on
    // the SD card, so a >4 GB torrent overruns FAT32's 4 GB file cap: writes
    // past it fail, the blocks are still marked present, and the piece can then
    // never verify -- it just gets handed back and re-claimed forever.
    int st_cache_wr_fail;
    int st_cache_rd_short;
    int64_t st_cache_written;  // bytes successfully written to the SD cache
    int st_live;             // peer sessions currently connected (handshaked)
    int st_peak_live;        // high-water mark of st_live
    char st_last_err[128];   // last fetch-failure reason
};

static void inc(torrentfs *t, int *c) {
    mutexLock(&t->lock);
    (*c)++;
    mutexUnlock(&t->lock);
}

// Per-block presence bitmap, so partial pieces survive a mid-piece choke and
// resume instead of restarting from scratch (the key to making progress on
// swarms that keep choking us). Global block index = piece*bpp + block.
static bool block_get(torrentfs *t, int64_t piece, int b) {
    int64_t bit = piece * t->blocks_per_piece + b;
    return (t->block_have[bit >> 3] & (1u << (bit & 7))) != 0;
}
static void block_set(torrentfs *t, int64_t piece, int b) {
    int64_t bit = piece * t->blocks_per_piece + b;
    t->block_have[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}
static void block_clear_piece(torrentfs *t, int64_t piece) {
    for (int b = 0; b < t->blocks_per_piece; b++) {
        int64_t bit = piece * t->blocks_per_piece + b;
        t->block_have[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
    }
}

// "Some session has this block on order." Purely an allocation hint: dropping a
// bit only risks fetching a block twice, never losing one.
static bool req_get(torrentfs *t, int64_t piece, int b) {
    int64_t bit = piece * t->blocks_per_piece + b;
    return (t->block_req[bit >> 3] & (1u << (bit & 7))) != 0;
}
static void req_set(torrentfs *t, int64_t piece, int b) {
    int64_t bit = piece * t->blocks_per_piece + b;
    t->block_req[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}
static void req_clear(torrentfs *t, int64_t piece, int b) {
    int64_t bit = piece * t->blocks_per_piece + b;
    t->block_req[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
}

// A piece is worth claiming if it still has a block nobody holds or has.
// Caller holds t->lock.
//
// `dup` ignores existing reservations, i.e. lets several sessions fetch the same
// blocks. That is waste on an ordinary piece, but the right trade for the few
// critical pieces: playback cannot start until the head/tail are in, and with
// reservations honoured there, a single slow peer that grabbed those blocks held
// up the whole stream with nobody allowed to help.
static bool piece_has_free_block_nolock(torrentfs *t, int64_t piece, int nblocks,
                                        bool dup) {
    for (int b = 0; b < nblocks; b++)
        if (!block_get(t, piece, b) && (dup || !req_get(t, piece, b))) return true;
    return false;
}

// The pieces *startup* is gated on: the file's first CRIT_HEAD and last
// CRIT_TAIL. Fetched redundantly so no single peer can stall the player before
// it has its header.
//
// Only during startup, though. Once the playhead has moved past the head the
// player clearly found its header (a faststart MP4 keeps the moov at the front
// and never touches the tail), and the tail becomes an ordinary piece. Treating
// it as critical forever was ruinous: claim_piece offers the tail first, so
// every session piled onto a piece nobody needed and duplicated its blocks
// endlessly -- 80% of all traffic wasted while the player starved at buf=0.
static bool piece_is_critical(torrentfs *t, int64_t i) {
    int64_t lo = t->file_first_piece, fhi = t->file_last_piece;
    if (lo < 0) lo = 0;

    // A piece whose reservations expired without delivering: race several peers
    // for it. Exclusive reservations otherwise make it a single point of
    // failure -- one peer that takes the blocks and goes quiet freezes playback
    // (measured: have=0/22 req=22 for 12s+, speed flat zero, a dozen peers
    // idle), and merely expiring the reservation hands the same deadlock to the
    // next peer. Scoped to stalled pieces only: doing this for every playhead
    // piece put 45 sessions on one piece and wasted 79% of the bandwidth.
    if (t->piece_stalled[i]) return true;

    if (t->playhead_piece > lo + CRIT_HEAD) return false;  // streaming: past startup
    return (i < lo + CRIT_HEAD) || (i > fhi - CRIT_TAIL);
}

//-----------------------------------------------------------------------------
// Chunked cache I/O
//
// The cache is addressed by absolute torrent offset; these map that onto
// CACHE_CHUNK-sized files so no single file ever approaches FAT32's 4 GB cap.
// Callers hold t->cache_lock.
//-----------------------------------------------------------------------------

// Opens chunk `ci` on first use. Returns NULL if it cannot be created.
static FILE *cache_chunk(torrentfs *t, int ci) {
    if (ci < 0 || ci >= CACHE_MAX_CHUNKS) return NULL;
    if (t->chunks[ci]) return t->chunks[ci];

    char path[256];
    snprintf(path, sizeof(path), "%s.%03d", t->cache_base, ci);
    FILE *f = fopen(path, "rb+");   // keep whatever a previous run left
    if (!f) f = fopen(path, "wb+"); // first use: create it
    t->chunks[ci] = f;
    return f;
}

// Reads/writes `len` bytes at absolute offset `off`, spanning chunk boundaries
// as needed. Returns false if any part of the transfer didn't happen -- callers
// must honour that: a block marked present but never written makes its piece
// permanently unverifiable.
static bool cache_io(torrentfs *t, int64_t off, void *buf, size_t len, bool write) {
    uint8_t *p = buf;
    while (len > 0) {
        int ci      = (int)(off / CACHE_CHUNK);
        int64_t co  = off % CACHE_CHUNK;
        size_t n    = len;
        if (co + (int64_t)n > CACHE_CHUNK)
            n = (size_t)(CACHE_CHUNK - co);  // split across the boundary

        FILE *f = cache_chunk(t, ci);
        if (!f) return false;
        if (fseek(f, co, SEEK_SET) != 0) return false;
        size_t done = write ? fwrite(p, 1, n, f) : fread(p, 1, n, f);
        if (done != n) return false;

        p += n;
        off += (int64_t)n;
        len -= n;
    }
    return true;
}

// Like cache_io(read) but returns how many bytes it actually got instead of
// all-or-nothing. The player's read path must never turn a short read into an
// error: mpv treats an error as a dead stream and stops reading for good, which
// also freezes the playhead and with it the whole download.
static size_t cache_read_upto(torrentfs *t, int64_t off, void *buf, size_t len) {
    uint8_t *p   = buf;
    size_t total = 0;
    while (len > 0) {
        int ci     = (int)(off / CACHE_CHUNK);
        int64_t co = off % CACHE_CHUNK;
        size_t n   = len;
        if (co + (int64_t)n > CACHE_CHUNK) n = (size_t)(CACHE_CHUNK - co);

        FILE *f = cache_chunk(t, ci);
        if (!f) break;
        if (fseek(f, co, SEEK_SET) != 0) break;
        size_t done = fread(p, 1, n, f);
        total += done;
        if (done != n) break;

        p += n;
        off += (int64_t)n;
        len -= n;
    }
    return total;
}

static void cache_flush(torrentfs *t) {
    for (int i = 0; i < CACHE_MAX_CHUNKS; i++)
        if (t->chunks[i]) fflush(t->chunks[i]);
}

//-----------------------------------------------------------------------------
// Piece bookkeeping
//-----------------------------------------------------------------------------

static bool have_piece(torrentfs *t, int64_t idx) {
    mutexLock(&t->lock);
    bool done = t->status[idx] == PIECE_DONE;
    mutexUnlock(&t->lock);
    return done;
}

// True if the piece already has at least one downloaded block. Caller holds
// t->lock (so it must not take it again).
static bool piece_partial_nolock(torrentfs *t, int64_t piece) {
    for (int b = 0; b < t->blocks_per_piece; b++) {
        int64_t bit = piece * t->blocks_per_piece + b;
        if (t->block_have[bit >> 3] & (1u << (bit & 7))) return true;
    }
    return false;
}

// Picks a piece for this peer. Pieces are shared, not owned: any piece the peer
// has that still contains a block nobody already holds or has fetched is fair
// game, in streaming order (critical head/tail first, then from the playhead).
//
// This used to hand each session an exclusive piece, with redundant "rescue"
// claims once the window saturated. With dozens of peers that meant ~6 sessions
// duplicating the same blocks of the same piece, so adding peers made
// throughput *worse*. Block-level claiming (see block_req) lets them split one
// piece instead -- and that piece is the one the player is blocked on.
static int64_t claim_piece(torrentfs *t, const uint8_t *pbf, size_t pbf_len) {
    int64_t idx = -1;
    mutexLock(&t->lock);
    int64_t pc = t->meta.piece_count;
    // Only ever claim pieces overlapping the target (video) file, so unrelated
    // files in the torrent are never downloaded.
    int64_t lo  = t->file_first_piece;
    int64_t fhi = t->file_last_piece;
    if (lo < 0) lo = 0;
    if (fhi >= pc) fhi = pc - 1;
    int64_t ph = t->playhead_piece;
    if (ph < lo) ph = lo;
    if (ph > fhi) ph = fhi;

    // Read-ahead window, in pieces (see STREAM_MIN_PIECES).
    int64_t window = (int64_t)(STREAM_WINDOW / t->meta.piece_len);
    if (window < STREAM_MIN_PIECES) window = STREAM_MIN_PIECES;
    int64_t hi = ph + window;
    if (hi > fhi + 1) hi = fhi + 1;

#define TFS_CLAIMABLE(i)                                                       \
    (t->status[(i)] != PIECE_DONE && bf_has_piece(pbf, pbf_len, (i)) &&        \
     piece_has_free_block_nolock(                                              \
         t, (i),                                                               \
         (int)((torrent_piece_len(&t->meta, (i)) + BLOCK_LEN - 1) / BLOCK_LEN),\
         piece_is_critical(t, (i))))

    // 0. Startup only: the file's last CRIT_TAIL then first CRIT_HEAD pieces.
    //    Tail first because an MP4's moov atom may live there and the player
    //    seeks to it before it can start.
    //
    //    Gated on the playhead still being at the head: once it has moved on,
    //    the player has its header and the tail is just another piece. Offering
    //    the tail first forever meant every session chased pieces nobody needed
    //    while the player starved on the ones it was actually reading.
    if (ph <= lo + CRIT_HEAD) {
        for (int64_t i = fhi; i > fhi - CRIT_TAIL && i >= lo && idx < 0; i--)
            if (TFS_CLAIMABLE(i)) idx = i;
        for (int64_t i = lo; i < lo + CRIT_HEAD && i <= fhi && idx < 0; i++)
            if (TFS_CLAIMABLE(i)) idx = i;
    }

    // 1. Streaming order: the lowest unfinished piece at/after the playhead --
    //    exactly the one the player is waiting for.
    for (int64_t i = ph; i < hi && idx < 0; i++)
        if (TFS_CLAIMABLE(i)) idx = i;

    // 2. Fallback: anything still missing before the playhead.
    for (int64_t i = lo; i < ph && idx < 0; i++)
        if (TFS_CLAIMABLE(i)) idx = i;

#undef TFS_CLAIMABLE

    if (idx >= 0 && t->status[idx] != PIECE_DONE) t->status[idx] = PIECE_INFLIGHT;

    // Record what this search actually looked at, so an empty result stays
    // explainable from the panel instead of by guesswork.
    t->st_win_ph = ph;
    t->st_win_lo = lo;
    t->st_win_hi = hi;
    if (idx < 0) t->st_claim_fail++; else t->st_claim_ok++;
    int inflight = 0;
    for (int64_t i = lo; i <= fhi; i++)
        if (t->status[i] == PIECE_INFLIGHT) inflight++;
    t->st_inflight = inflight;

    mutexUnlock(&t->lock);
    return idx;
}

static void set_status(torrentfs *t, int64_t idx, uint8_t st) {
    mutexLock(&t->lock);
    // PIECE_DONE is final: it means the piece is on disk and its SHA-1 checked
    // out. Letting a caller downgrade it was a silent disaster -- pieces are
    // shared by many sessions, so when one finished a piece and another still
    // holding it then died or got choked, that second session handed the
    // finished piece back as PIECE_NEEDED. It was re-claimed, re-downloaded and
    // re-verified forever (double-counting pieces_done past 100% on the way),
    // and the player never saw a stable copy of the header piece it was
    // blocked on.
    if (t->status[idx] != PIECE_DONE)
        t->status[idx] = st;
    mutexUnlock(&t->lock);
}

static bool all_done(torrentfs *t) {
    mutexLock(&t->lock);
    bool done = t->pieces_done >= t->meta.piece_count;
    mutexUnlock(&t->lock);
    return done;
}

//-----------------------------------------------------------------------------
// Download workers
//-----------------------------------------------------------------------------

// Per-peer retry backoff. A failed connect benches the peer for a while,
// doubling with each consecutive failure, so dead/NAT'd addresses get retried
// ever more rarely instead of continuously -- yet never permanently, since
// until the next discovery round this list is all we have. A peer that DID
// connect gets a short fixed pause instead: reconnecting to a seeder the
// instant it drops us reads as hammering, and real clients ban IPs that
// reconnect in a loop -- which quietly shrank the set of peers still willing
// to talk to us until the download flatlined.
#define PEER_BACKOFF_CONN_SECS 15   // first failure; doubles up to the cap
#define PEER_BACKOFF_MAX_SECS  300
#define PEER_BACKOFF_DROP_SECS 30   // after a connection that had succeeded

// Hands out the next eligible peer (round-robin), skipping peers a session
// already holds and peers still inside their retry backoff. Returns the peer
// index, or -1 if none is usable right now -- which is fine: the loop asks
// again every pass, and backoffs expire on their own.
static int take_peer(torrentfs *t, peer_addr *out) {
    u64 now = armGetSystemTick();
    mutexLock(&t->lock);
    int idx = -1;
    if (!t->stop && t->peer_count > 0) {
        for (int tries = 0; tries < t->peer_count; tries++) {
            int i = t->next_peer++ % t->peer_count;
            // Never hand out a peer a session already holds. The session
            // loop keeps ~MAX_SESS slots full and recycles them fast, so
            // without this the round-robin wraps past peer_count and opens
            // a SECOND connection to a peer we are already talking to --
            // which every client drops as a duplicate. That killed sessions
            // ~0.4 s after handshake and burned 5745 connects in 2 minutes.
            if (t->peer_busy[i]) continue;
            if (t->peer_next_try[i] > now) continue;  // still benched
            idx = i;
            *out = t->peers[i];
            t->peer_busy[i] = 1;
            break;
        }
    }
    mutexUnlock(&t->lock);
    return idx;
}

// Length of block `b` within a piece of length `plen`.
static uint32_t block_len_of(int64_t plen, int b) {
    int64_t begin = (int64_t)b * BLOCK_LEN;
    int64_t rem = plen - begin;
    return rem > BLOCK_LEN ? BLOCK_LEN : (uint32_t)rem;
}

// Tells a peer to drop a request we no longer want. Without this, every block
// we abandon (piece finished by someone else, or we got choked) still arrives
// and is thrown away -- paid for, unusable, and counted as duplicate traffic.
static void send_cancel(peer_nb *p, int64_t index, uint32_t begin, uint32_t len) {
    uint8_t req[12];
    uint32_t v;
    v = htonl((uint32_t)index); memcpy(req + 0, &v, 4);
    v = htonl(begin);           memcpy(req + 4, &v, 4);
    v = htonl(len);             memcpy(req + 8, &v, 4);
    peer_nb_queue(p, MSG_CANCEL, req, 12);
}

static void send_request(peer_nb *p, int64_t index, uint32_t begin, uint32_t len) {
    uint8_t req[12];
    uint32_t v;
    v = htonl((uint32_t)index); memcpy(req + 0, &v, 4);
    v = htonl(begin);           memcpy(req + 4, &v, 4);
    v = htonl(len);             memcpy(req + 8, &v, 4);
    peer_nb_queue(p, MSG_REQUEST, req, 12);
}

// Builds a bitfield of the pieces we currently have, to advertise to a peer.
static void build_our_bitfield(torrentfs *t, uint8_t *bf, size_t bf_len) {
    memset(bf, 0, bf_len);
    mutexLock(&t->lock);
    for (int64_t i = 0; i < t->meta.piece_count; i++)
        if (t->status[i] == PIECE_DONE) bf[i / 8] |= (uint8_t)(0x80 >> (i % 8));
    mutexUnlock(&t->lock);
}

// A piece's blocks are written to the cache as they arrive (so partial progress
// survives a choke). Once all blocks are present, read the whole piece back from
// the cache, verify its SHA-1, and on success mark it done. On failure, clear
// the piece's block bitmap so it is re-downloaded. `buf` is scratch (>= plen).
static bool verify_and_finish(torrentfs *t, int64_t idx, int64_t plen, uint8_t *buf) {
    mutexLock(&t->cache_lock);
    cache_flush(t);  // make sure this piece's buffered block writes are visible
    bool read_ok = cache_io(t, idx * t->meta.piece_len, buf, (size_t)plen, false);
    mutexUnlock(&t->cache_lock);
    size_t got = read_ok ? (size_t)plen : 0;
    if (got != (size_t)plen) {
        // Silent before: no counter, no error, piece just handed back -- so this
        // looped forever with sha_fail/fetch_fail both reading zero.
        set_status(t, idx, PIECE_NEEDED);
        mutexLock(&t->lock);
        t->st_cache_rd_short++;
        snprintf(t->st_last_err, sizeof(t->st_last_err),
                 "cache read %zu/%lld @%lld MB", got, (long long)plen,
                 (long long)((idx * t->meta.piece_len) >> 20));
        mutexUnlock(&t->lock);
        return false;
    }

    uint8_t hash[20];
    mbedtls_sha1(buf, plen, hash);
    if (memcmp(hash, t->meta.piece_hashes + idx * 20, 20) != 0) {
        mutexLock(&t->lock);
        block_clear_piece(t, idx);   // corrupt: drop partial, re-download
        mutexUnlock(&t->lock);
        set_status(t, idx, PIECE_NEEDED);
        inc(t, &t->st_sha_fail);
        return false;
    }

    mutexLock(&t->lock);
    if (t->status[idx] != PIECE_DONE) {
        t->status[idx] = PIECE_DONE;
        t->pieces_done++;
    }
    t->piece_stalled[idx] = 0;  // done: no reason to race for it any more
    t->st_piece_ok++;
    mutexUnlock(&t->lock);
    return true;
}

// Serves one block to a peer that requested it, if we hold that piece. `sbuf`
// is a scratch buffer of at least 8 + BLOCK_LEN bytes.
static void serve_block(torrentfs *t, peer_nb *p, uint32_t index,
                        uint32_t begin, uint32_t len, uint8_t *sbuf) {
    if (len == 0 || len > BLOCK_LEN) return;
    if (index >= (uint32_t)t->meta.piece_count) return;
    if (begin + len > (uint32_t)torrent_piece_len(&t->meta, index)) return;
    if (!have_piece(t, index)) return;

    uint32_t v;
    v = htonl(index); memcpy(sbuf + 0, &v, 4);
    v = htonl(begin); memcpy(sbuf + 4, &v, 4);

    mutexLock(&t->cache_lock);
    bool ok = cache_io(t, (int64_t)index * t->meta.piece_len + begin, sbuf + 8,
                       len, false);
    mutexUnlock(&t->cache_lock);
    if (!ok) return;

    if (peer_nb_queue(p, MSG_PIECE, sbuf, 8 + len) == 0)
        inc(t, &t->st_blocks_served);
}

// Advertises (via HAVE) every piece we now hold that this session has not yet
// told the peer about. Each session tracks its own `adv` bitfield, so pieces
// completed by OTHER workers get advertised here too — otherwise a peer only
// ever learns about the pieces the one worker talking to it happened to finish,
// and never requests (nor reciprocates for) the rest. Bounded per call so a big
// catch-up does not stall the session. Returns the number of HAVEs sent.
static int sync_haves(torrentfs *t, peer_nb *p, uint8_t *adv, int64_t pc) {
    int sent = 0;
    for (int64_t i = 0; i < pc && sent < 64; i++) {
        uint8_t bit = (uint8_t)(0x80 >> (i % 8));
        if (adv[i / 8] & bit) continue;              // already advertised
        mutexLock(&t->lock);
        bool done = t->status[i] == PIECE_DONE;
        mutexUnlock(&t->lock);
        if (!done) continue;
        adv[i / 8] |= bit;
        uint8_t hv[4];
        uint32_t v = htonl((uint32_t)i);
        memcpy(hv, &v, 4);
        peer_nb_queue(p, MSG_HAVE, hv, 4);
        sent++;
    }
    return sent;
}

// Runs a full bidirectional session on a connected peer: downloads pieces we
// need AND serves pieces we have to the peer, so it reciprocates (tit-for-tat)
// and keeps us unchoked instead of dropping a non-uploading leech. `buf` sizes a
// full piece. Returns when the connection drops or we stop.
//-----------------------------------------------------------------------------
// Peer session loop (poll-driven)
//
// One thread drives every peer. The old design gave each peer its own thread
// parked in a blocking recv(), and libnx hands out only 16 BSD sessions
// (NX_SESSION_MGR_MAX_SESSIONS) -- one per concurrent blocking call -- so it
// could never hold more than ~11 peers at once. As a 0%-complete leecher we
// have nothing to trade, so we live entirely off optimistic unchokes, which a
// seeder hands to ~4 peers at a time out of hundreds: with 11 connections we
// caught one or two, while a PC client holding 200 catches dozens. That gap,
// not the swarm, is why large torrents never buffered.
//
// poll() is a single IPC call regardless of how many sockets it watches, so the
// whole set costs ONE session and the peer count stops being bounded by libnx.
//-----------------------------------------------------------------------------

// Concurrent peer sessions. Bounded by memory (each holds a ~17 KB rx buffer),
// not by BSD sessions any more.
// Kept under what the socket buffer pool can actually hand out: the pool is
// sized for the nvtegra video decoder to still get its memory (see
// switch_wrapper.c), and asking for more than it holds just makes socket()
// return ENOBUFS on every extra peer. ~50 concurrent peers is already 4x what
// the old thread-per-peer design could reach.
#define MAX_SESS 48

// Drop a peer that has sent us nothing at all for this long.
#define SESS_IDLE_SECS 20

// Give up on a session's *block reservations* well before we give up on the
// session itself. A reservation locks other peers out of those blocks, so a
// peer that accepted requests and then went quiet takes them hostage: the last
// few blocks of a piece sit unfetchable while a dozen willing peers idle, the
// player blocks on that piece, and the buffer drains to zero. Nothing else
// expires reservations, so without this a single slow peer stalls the stream.
#define BLOCK_STALL_SECS 5

// Cap on slots sitting in a pending outbound connect. On a big swarm most peers
// are NAT'd or dead, and each one holds its slot for PEER_CONNECT_SECS before it
// can be reaped -- measured at ~6000 timeouts to ~500 successes, enough to pin
// ~34 slots permanently. Without this bound the dead peers crowd out both the
// live sessions and the incoming peers (which are reachable by definition), so
// throughput starts well and then decays as the reachable peers get used up.
#define MAX_CONNECTING 24

typedef struct {
    peer_nb nb;
    bool active;
    bool dead;
    bool connecting;      // non-blocking connect still in flight
    bool counted_unchoke;
    int pidx;             // index into t->peers, or -1 for an incoming peer

    int64_t dl_idx;       // piece being downloaded, or -1
    int64_t dl_plen;
    int nblocks, received, req_cursor;
    bool *got;
    bool *mine;           // blocks of dl_idx this session reserved in block_req

    uint8_t *adv;         // pieces this session has advertised to the peer
    u64 last_adv;
    u64 started;          // tick the connect was issued
    u64 last_rx;          // tick of the last byte received
    u64 last_block;       // tick of the last *block* received (reservation life)
} sess;

static void sess_reset(torrentfs *t, sess *s, bool conn_failed) {
    // Hand back every block we had on order, or they stay reserved forever and
    // no other session can ever fetch them -- the piece would stall at 99%.
    if (s->dl_idx >= 0 && s->mine) {
        mutexLock(&t->lock);
        for (int b = 0; b < s->nblocks; b++)
            if (s->mine[b]) req_clear(t, s->dl_idx, b);
        mutexUnlock(&t->lock);
    }
    if (s->dl_idx >= 0) set_status(t, s->dl_idx, PIECE_NEEDED);  // blocks kept
    if (s->pidx >= 0) {
        u64 freq = armGetSystemTickFreq();
        u64 backoff;
        mutexLock(&t->lock);
        t->peer_busy[s->pidx] = 0;  // back into the rotation, after a pause
        if (conn_failed) {
            t->st_conn_fail++;
            if (t->peer_fails[s->pidx] < 255) t->peer_fails[s->pidx]++;
            int f = t->peer_fails[s->pidx];
            if (f > 6) f = 6;  // keeps the shift sane; the cap below rules anyway
            backoff = (u64)PEER_BACKOFF_CONN_SECS << (f - 1);
            if (backoff > PEER_BACKOFF_MAX_SECS) backoff = PEER_BACKOFF_MAX_SECS;
        } else {
            // The connect had succeeded (peer_fails was reset in
            // sess_connected); the pause only spaces out our reconnects.
            backoff = PEER_BACKOFF_DROP_SECS;
        }
        t->peer_next_try[s->pidx] = armGetSystemTick() + backoff * freq;
        mutexUnlock(&t->lock);
    }
    peer_nb_free(&s->nb);
    free(s->got);
    free(s->mine);
    free(s->adv);
    memset(s, 0, sizeof(*s));
    s->nb.sock = -1;
    s->dl_idx  = -1;
}

// Starts a non-blocking connect into a free slot. Returns false if there is no
// peer to try right now (the caller just leaves the slot empty).
static bool sess_open(torrentfs *t, sess *s) {
    peer_addr addr;
    int pidx = take_peer(t, &addr);
    if (pidx < 0) return false;

    // take_peer marked pidx busy; every failure path below has to hand it back
    // or the peer is benched for good and the pool bleeds out.
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        mutexLock(&t->lock);
        t->peer_busy[pidx] = 0;
        // Not a dead peer: we could not even make a socket. On Switch every TCP
        // socket draws its buffers from a fixed pool sized at socketInitialize
        // time, so this is how running out of that pool shows up.
        t->st_sock_fail++;
        snprintf(t->st_last_err, sizeof(t->st_last_err), "socket(): %s",
                 strerror(errno));
        mutexUnlock(&t->lock);
        return false;
    }
    if (peer_nb_init(&s->nb, sock, t->meta.piece_count) != 0) {
        close(sock);
        mutexLock(&t->lock);
        t->peer_busy[pidx] = 0;
        mutexUnlock(&t->lock);
        return false;
    }

    struct sockaddr_in sa = { 0 };
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(addr.port);
    sa.sin_addr.s_addr = addr.ip;

    int rc = connect(s->nb.sock, (struct sockaddr *)&sa, sizeof(sa));
    if (rc != 0 && errno != EINPROGRESS) {
        int e = errno;
        peer_nb_free(&s->nb);
        mutexLock(&t->lock);
        t->peer_busy[pidx] = 0;
        t->st_conn_fail++;
        if (t->peer_fails[pidx] < 255) t->peer_fails[pidx]++;
        // Distinguishes "this peer is unreachable" from "we are out of socket
        // buffers" -- both land here, but only one is the swarm's fault.
        t->st_sock_fail++;
        snprintf(t->st_last_err, sizeof(t->st_last_err), "connect(): %s",
                 strerror(e));
        mutexUnlock(&t->lock);
        return false;
    }

    s->active     = true;
    s->dead       = false;
    s->connecting = (rc != 0);
    s->pidx       = pidx;
    s->dl_idx     = -1;
    s->started    = armGetSystemTick();
    s->last_rx    = s->started;
    return true;
}

// The TCP connection is up: send our handshake. The bitfield and interested go
// out once the peer's handshake comes back.
static void sess_connected(torrentfs *t, sess *s) {
    mutexLock(&t->lock);
    t->st_conn_ok++;
    if (s->pidx >= 0) t->peer_fails[s->pidx] = 0;  // reachable: keep in rotation
    mutexUnlock(&t->lock);
    if (peer_nb_send_handshake(&s->nb, t->meta.info_hash, t->peer_id) != 0)
        s->dead = true;
}

// Claim a piece this peer has and pipeline the first window of requests.
static void sess_try_claim(torrentfs *t, sess *s, uint8_t *buf) {
    if (s->nb.choked || s->dl_idx >= 0 || !s->nb.handshaked) return;

    int64_t idx = claim_piece(t, s->nb.bitfield, s->nb.bitfield_len);
    if (idx < 0) return;

    s->dl_idx  = idx;
    s->dl_plen = torrent_piece_len(&t->meta, idx);
    s->nblocks = (int)((s->dl_plen + BLOCK_LEN - 1) / BLOCK_LEN);
    free(s->got);
    free(s->mine);
    s->got  = calloc(s->nblocks, 1);
    s->mine = calloc(s->nblocks, 1);
    if (!s->got || !s->mine) {
        set_status(t, idx, PIECE_NEEDED);
        s->dl_idx = -1;
        return;
    }

    // Resume: blocks fetched earlier (by us or another session) are kept.
    s->received = 0;
    int queued = 0;
    mutexLock(&t->lock);
    for (int b = 0; b < s->nblocks; b++)
        if (block_get(t, idx, b)) { s->got[b] = true; s->received++; }

    // Take only blocks nobody else has on order, so sessions sharing this piece
    // split it instead of all fetching the same bytes. `mine` records what we
    // reserved, so sess_reset can hand it back if we die mid-piece.
    //
    // Critical pieces are the exception: there we deliberately duplicate, since
    // playback is blocked until they land and honouring reservations let one
    // slow peer own them with nobody able to help.
    bool dup = piece_is_critical(t, idx);
    for (int b = 0; b < s->nblocks && queued < SESS_PIPELINE; b++) {
        if (s->got[b]) continue;
        if (!dup && req_get(t, idx, b)) continue;
        req_set(t, idx, b);
        s->mine[b] = true;
        queued++;
    }
    mutexUnlock(&t->lock);

    if (s->received == s->nblocks) {  // already complete, just verify
        verify_and_finish(t, s->dl_idx, s->dl_plen, buf);
        s->dl_idx = -1;
        return;
    }
    if (queued == 0) {
        // Every missing block is already on order elsewhere; leave this piece to
        // them and look again next pass rather than duplicating the work.
        s->dl_idx = -1;
        return;
    }

    // Start the reservation clock now, not on the first block: a peer that
    // takes our requests and never answers is exactly the case that strands
    // blocks, and it would never set last_block at all.
    s->last_block = armGetSystemTick();
    for (int b = 0; b < s->nblocks; b++)
        if (s->mine[b])
            send_request(&s->nb, s->dl_idx, (uint32_t)b * BLOCK_LEN,
                         block_len_of(s->dl_plen, b));
    s->req_cursor = s->nblocks;  // window refills come from sess_msg
}

static void sess_msg(torrentfs *t, sess *s, uint8_t id, uint8_t *payload,
                     uint32_t mplen, uint8_t *buf, uint8_t *sbuf) {
    int64_t pc     = t->meta.piece_count;
    size_t bf_len  = s->nb.bitfield_len;

    if (id == MSG_UNCHOKE) {
        s->nb.choked = false;
        if (!s->counted_unchoke) {
            inc(t, &t->st_unchoke_ok);
            s->counted_unchoke = true;
        }
    } else if (id == MSG_CHOKE) {
        s->nb.choked = true;
        if (s->dl_idx >= 0) {  // hand the (partial) piece back; blocks are kept
            set_status(t, s->dl_idx, PIECE_NEEDED);
            mutexLock(&t->lock);
            // Release our block reservations too. Dropping the piece without
            // this strands them: no other session may take those blocks, so the
            // piece can never be completed by anyone.
            if (s->mine)
                for (int i = 0; i < s->nblocks; i++)
                    if (s->mine[i]) { s->mine[i] = false; req_clear(t, s->dl_idx, i); }
            t->st_fetch_fail++;
            snprintf(t->st_last_err, sizeof(t->st_last_err), "choke");
            mutexUnlock(&t->lock);
            s->dl_idx = -1;
        }
        // Stay connected and re-assert interest so the peer unchokes us on its
        // next rotation; we keep serving its requests, which earns that.
        peer_nb_queue(&s->nb, MSG_INTERESTED, NULL, 0);
    } else if (id == MSG_INTERESTED) {
        inc(t, &t->st_interested_recv);
        if (!s->nb.we_unchoked) {
            peer_nb_queue(&s->nb, MSG_UNCHOKE, NULL, 0);
            s->nb.we_unchoked = true;
        }
    } else if (id == MSG_HAVE) {
        if (mplen == 4 && s->nb.bitfield) {
            uint32_t hi;
            memcpy(&hi, payload, 4);
            hi = ntohl(hi);
            if (hi < (uint32_t)pc)
                s->nb.bitfield[hi / 8] |= (uint8_t)(0x80 >> (hi % 8));
        }
    } else if (id == MSG_BITFIELD) {
        // A rejected bitfield is invisible but fatal: with no bits set, every
        // bf_has_piece() is false, claim_piece() finds nothing for that peer,
        // and the session sits unchoked and idle forever. Count both outcomes so
        // a length mismatch can't hide.
        if (s->nb.bitfield && mplen == bf_len) {
            memcpy(s->nb.bitfield, payload, bf_len);
            inc(t, &t->st_bf_ok);
        } else {
            mutexLock(&t->lock);
            t->st_bf_bad++;
            snprintf(t->st_last_err, sizeof(t->st_last_err),
                     "bitfield %u B, expected %u", (unsigned)mplen,
                     (unsigned)bf_len);
            mutexUnlock(&t->lock);
        }
    } else if (id == MSG_REQUEST) {
        inc(t, &t->st_request_recv);
        if (s->nb.we_unchoked && mplen == 12) {
            uint32_t ri, rb, rl;
            memcpy(&ri, payload + 0, 4); ri = ntohl(ri);
            memcpy(&rb, payload + 4, 4); rb = ntohl(rb);
            memcpy(&rl, payload + 8, 4); rl = ntohl(rl);
            serve_block(t, &s->nb, ri, rb, rl, sbuf);
        }
    } else if (id == MSG_PIECE && s->dl_idx >= 0 && mplen >= 8) {
        uint32_t ri, rb;
        memcpy(&ri, payload + 0, 4); ri = ntohl(ri);
        memcpy(&rb, payload + 4, 4); rb = ntohl(rb);
        uint32_t blen = mplen - 8;
        if (ri != (uint32_t)s->dl_idx || rb % BLOCK_LEN != 0) return;

        int b = (int)(rb / BLOCK_LEN);
        if (b < 0 || b >= s->nblocks || s->got[b] ||
            blen != block_len_of(s->dl_plen, b))
            return;

        // Persist the block immediately so it survives a later choke/disconnect.
        // Check the write: marking a block present that never reached the disk
        // makes the piece unverifiable forever (it reads back short), and the
        // engine then re-claims it endlessly with no error anywhere.
        int64_t off = s->dl_idx * t->meta.piece_len + rb;
        mutexLock(&t->cache_lock);
        bool wrote = cache_io(t, off, payload + 8, blen, true);
        mutexUnlock(&t->cache_lock);
        if (!wrote) {
            mutexLock(&t->lock);
            t->st_cache_wr_fail++;
            snprintf(t->st_last_err, sizeof(t->st_last_err),
                     "cache write failed @%lld MB", (long long)(off >> 20));
            mutexUnlock(&t->lock);
            return;  // do NOT mark it present
        }
        mutexLock(&t->lock);
        block_set(t, s->dl_idx, b);
        t->st_bytes_recv += blen;
        t->st_cache_written += blen;
        mutexUnlock(&t->lock);

        s->got[b] = true;
        s->received++;
        s->last_block = armGetSystemTick();  // this session is still delivering

        // This block is settled: release our reservation and take one more that
        // nobody else holds, keeping the pipeline full without duplicating what
        // the other sessions on this piece are already fetching.
        mutexLock(&t->lock);
        if (s->mine[b]) { s->mine[b] = false; req_clear(t, s->dl_idx, b); }
        bool dup = piece_is_critical(t, s->dl_idx);
        int next = -1;
        for (int i = 0; i < s->nblocks; i++) {
            // Must test the *global* bitmap, not just s->got: s->got is a
            // snapshot from claim time, so a block another session has fetched
            // and released since reads as "missing and unreserved" here. That
            // had every session on a piece re-requesting blocks already on
            // disk -- each block was fetched ~6x and ~80% of all traffic was
            // thrown away.
            if (s->got[i] || block_get(t, s->dl_idx, i)) continue;
            if (!dup && req_get(t, s->dl_idx, i)) continue;
            req_set(t, s->dl_idx, i);
            s->mine[i] = true;
            next = i;
            break;
        }
        // Completion is a property of the piece, not of this session. Sessions
        // split a piece between them, so none of them ever receives every block
        // itself -- checking s->received meant a finished piece went unnoticed
        // until some session happened to re-claim it and find it already whole.
        bool whole = true;
        for (int i = 0; i < s->nblocks && whole; i++)
            if (!block_get(t, s->dl_idx, i)) whole = false;
        mutexUnlock(&t->lock);

        if (next >= 0)
            send_request(&s->nb, s->dl_idx, (uint32_t)next * BLOCK_LEN,
                         block_len_of(s->dl_plen, next));

        // Every session sharing this piece sees `whole` on the same block, and
        // verify_and_finish re-reads 4 MB off the SD card and SHA-1s it while
        // holding cache_lock -- doing that once per session would stall all the
        // others. Claim the verify so only one of them does it.
        bool mine_to_verify = false;
        mutexLock(&t->lock);
        if (whole && t->status[s->dl_idx] != PIECE_DONE &&
            t->status[s->dl_idx] != PIECE_VERIFYING) {
            t->status[s->dl_idx] = PIECE_VERIFYING;
            mine_to_verify       = true;
        }
        mutexUnlock(&t->lock);

        // Release on `whole`, NOT on `mine_to_verify`: only one session verifies
        // the piece, but every session sharing it is finished with it. Gating
        // the release on winning the verify race left the others holding a
        // completed piece forever -- they never claimed another, and their
        // outstanding requests kept arriving and being re-written over blocks
        // already on disk, which was ~79% of all traffic.
        if (whole) {
            if (mine_to_verify && verify_and_finish(t, s->dl_idx, s->dl_plen, buf)) {
                uint8_t hv[4];
                uint32_t v = htonl((uint32_t)s->dl_idx);
                memcpy(hv, &v, 4);
                peer_nb_queue(&s->nb, MSG_HAVE, hv, 4);  // advertise it
                if (s->adv) s->adv[s->dl_idx / 8] |= (uint8_t)(0x80 >> (s->dl_idx % 8));
            }
            // Drop any reservation we still held on this piece, and cancel just
            // those requests -- they are the ones still on their way to us, and
            // would otherwise arrive only to be thrown away. Only the blocks we
            // actually asked for: cancelling every missing block flooded the
            // peer with hundreds of pointless messages.
            mutexLock(&t->lock);
            int cancel[SESS_PIPELINE];
            int ncancel = 0;
            for (int i = 0; i < s->nblocks; i++) {
                if (!s->mine[i]) continue;
                s->mine[i] = false;
                req_clear(t, s->dl_idx, i);
                if (!s->got[i] && ncancel < SESS_PIPELINE) cancel[ncancel++] = i;
            }
            mutexUnlock(&t->lock);
            for (int i = 0; i < ncancel; i++)
                send_cancel(&s->nb, s->dl_idx, (uint32_t)cancel[i] * BLOCK_LEN,
                            block_len_of(s->dl_plen, cancel[i]));
            s->dl_idx = -1;
        }
    }
}

// Reads whatever arrived and runs the session forward. Never blocks.
static void sess_service(torrentfs *t, sess *s, uint8_t *buf, uint8_t *sbuf) {
    ssize_t got = peer_nb_pump_rx(&s->nb);
    if (got < 0) { s->dead = true; return; }
    // Only real bytes count as liveness -- a spurious poll wakeup must not keep
    // resetting the idle deadline, or a silent peer is never reaped.
    if (got > 0) s->last_rx = armGetSystemTick();

    if (!s->nb.handshaked) {
        int r = peer_nb_recv_handshake(&s->nb, t->meta.info_hash);
        if (r < 0) { s->dead = true; return; }
        if (r == 0) return;  // still arriving

        // Handshaked: advertise what we hold, then declare interest.
        size_t bf_len = s->nb.bitfield_len;
        s->adv = calloc(1, bf_len ? bf_len : 1);
        if (s->adv) {
            build_our_bitfield(t, s->adv, bf_len);
            peer_nb_queue(&s->nb, MSG_BITFIELD, s->adv, (uint32_t)bf_len);
        }
        peer_nb_queue(&s->nb, MSG_INTERESTED, NULL, 0);
        s->last_adv = armGetSystemTick();
    }

    uint8_t id;
    uint8_t *payload;
    uint32_t mplen;
    int r;
    while ((r = peer_nb_next(&s->nb, &id, &payload, &mplen)) == 1) {
        sess_msg(t, s, id, payload, mplen, buf, sbuf);
        if (s->dead) return;
    }
    if (r < 0) { s->dead = true; return; }

    sess_try_claim(t, s, buf);
}

static void netloop_main(void *arg) {
    torrentfs *t = arg;

    sess *S            = calloc(MAX_SESS, sizeof(sess));
    struct pollfd *pfd = calloc(MAX_SESS + 1, sizeof(struct pollfd));
    int *map           = calloc(MAX_SESS + 1, sizeof(int));
    uint8_t *buf       = malloc(t->meta.piece_len);   // verify scratch
    uint8_t *sbuf      = malloc(8 + BLOCK_LEN);       // serve scratch
    if (!S || !pfd || !map || !buf || !sbuf) goto out;

    for (int i = 0; i < MAX_SESS; i++) { S[i].nb.sock = -1; S[i].dl_idx = -1; }

    u64 freq = armGetSystemTickFreq();

    while (!t->stop && !all_done(t)) {
        // Adopt peers that connected to us (queued by the acceptor threads).
        // These are worth more than outbound attempts: a peer that reaches us is
        // by definition reachable, and on a NAT'd swarm they are often the only
        // seeds we can talk to at all.
        for (int i = 0; i < MAX_SESS; i++) {
            if (S[i].active) continue;
            int fd = -1;
            mutexLock(&t->lock);
            for (int q = 0; q < INCOMING_QUEUE; q++)
                if (t->incoming[q] >= 0) { fd = t->incoming[q]; t->incoming[q] = -1; break; }
            mutexUnlock(&t->lock);
            if (fd < 0) break;
            if (peer_nb_init(&S[i].nb, fd, t->meta.piece_count) != 0) {
                close(fd);
                continue;
            }
            S[i].active  = true;
            S[i].pidx    = -1;  // not from the tracker pool: no fail bookkeeping
            S[i].dl_idx  = -1;
            S[i].started = S[i].last_rx = armGetSystemTick();
            sess_connected(t, &S[i]);
            if (S[i].dead) sess_reset(t, &S[i], false);
        }

        // Fill the rest with outbound attempts, but never let more than
        // MAX_CONNECTING slots sit in a pending connect: on a swarm where most
        // peers are dead, unbounded attempts pin the whole table for a second at
        // a time and starve the live sessions and the incoming peers.
        int connecting = 0;
        for (int i = 0; i < MAX_SESS; i++)
            if (S[i].active && S[i].connecting) connecting++;
        for (int i = 0; i < MAX_SESS && connecting < MAX_CONNECTING; i++) {
            if (S[i].active) continue;
            if (!sess_open(t, &S[i])) break;  // no peer to try right now
            if (S[i].connecting) connecting++;
        }

        int n = 0;
        for (int i = 0; i < MAX_SESS; i++) {
            if (!S[i].active) continue;
            pfd[n].fd      = S[i].nb.sock;
            pfd[n].events  = POLLIN;
            if (S[i].connecting || peer_nb_tx_pending(&S[i].nb))
                pfd[n].events |= POLLOUT;
            pfd[n].revents = 0;
            map[n]         = i;
            n++;
        }
        if (n == 0) {  // nothing open yet (no peers known); wait for some
            svcSleepThread(200000000ULL);  // 200 ms
            continue;
        }

        // One IPC for the whole set -> one BSD session, however many peers.
        int rc = poll(pfd, (nfds_t)n, 200);
        if (rc < 0) { svcSleepThread(50000000ULL); continue; }

        for (int k = 0; k < n && !t->stop; k++) {
            sess *s   = &S[map[k]];
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
                if (getsockopt(s->nb.sock, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 ||
                    soerr != 0) {
                    sess_reset(t, s, true);
                    continue;
                }
                s->connecting = false;
                sess_connected(t, s);
                if (s->dead) { sess_reset(t, s, false); continue; }
            }
            if (rev & POLLOUT) {
                if (peer_nb_flush(&s->nb) != 0) { sess_reset(t, s, false); continue; }
            }
            if (rev & POLLIN) {
                sess_service(t, s, buf, sbuf);
                if (s->dead) { sess_reset(t, s, false); continue; }
            }
        }

        // Periodic upkeep: advertise new pieces, push queued bytes, reap peers
        // that went silent or never completed their connect.
        u64 now = armGetSystemTick();
        int live = 0, claiming = 0, idleUnchoked = 0, bfEmpty = 0;
        for (int i = 0; i < MAX_SESS; i++) {
            sess *s = &S[i];
            if (!s->active) continue;
            if (s->nb.handshaked) {
                live++;
                if (s->dl_idx >= 0)        claiming++;      // actually fetching
                else if (!s->nb.choked)    idleUnchoked++;  // allowed to ask, isn't
                // A peer we know nothing about can never be claimed against.
                bool any = false;
                for (size_t b = 0; b < s->nb.bitfield_len && !any; b++)
                    if (s->nb.bitfield[b]) any = true;
                if (!any) bfEmpty++;
            }

            if (s->connecting) {
                if (now - s->started >= freq * PEER_CONNECT_SECS) {
                    inc(t, &t->st_conn_timeout);  // SYN went out, nobody answered
                    sess_reset(t, s, true);
                }
                continue;
            }
            if (now - s->last_rx >= freq * SESS_IDLE_SECS) {
                sess_reset(t, s, false);
                continue;
            }
            if (s->adv && now - s->last_adv >= freq) {
                sync_haves(t, &s->nb, s->adv, t->meta.piece_count);
                s->last_adv = now;
            }

            // Expire reservations from a session that stopped delivering. It
            // keeps the connection (it may recover), but its blocks go back in
            // the pool so the idle peers can finish the piece the player is
            // waiting on.
            if (s->dl_idx >= 0 && s->mine && s->last_block &&
                now - s->last_block >= freq * BLOCK_STALL_SECS) {
                mutexLock(&t->lock);
                for (int i = 0; i < s->nblocks; i++)
                    if (s->mine[i]) { s->mine[i] = false; req_clear(t, s->dl_idx, i); }
                // Mark it stalled so peers may now race for it: just freeing the
                // reservation isn't enough -- the next session grabs the same
                // blocks and stalls identically.
                t->piece_stalled[s->dl_idx] = 1;
                mutexUnlock(&t->lock);
                set_status(t, s->dl_idx, PIECE_NEEDED);
                s->dl_idx     = -1;
                s->last_block = 0;
            }

            // Retry the claim here, every pass, for any unchoked session that
            // isn't downloading. This must NOT be driven off POLLIN only (as it
            // was): an unchoked peer whose claim_piece came back empty -- the
            // read-ahead window is just STREAM_WINDOW/piece_len pieces wide, so
            // it saturates fast -- has nothing to send us, precisely because we
            // never asked it for anything. No traffic means no POLLIN, which
            // means the claim was never retried, so the session sat idle for
            // good. Peers fell into that state one by one and throughput decayed
            // to zero and stayed there.
            if (s->nb.handshaked && !s->nb.choked && s->dl_idx < 0)
                sess_try_claim(t, s, buf);

            if (peer_nb_tx_pending(&s->nb) && peer_nb_flush(&s->nb) != 0)
                sess_reset(t, s, false);
        }

        // Concurrent handshaked peers -- the number this whole rewrite exists to
        // raise. The old thread-per-peer design could not exceed ~11.
        int pending = 0;
        for (int i = 0; i < MAX_SESS; i++)
            if (S[i].active && S[i].connecting) pending++;
        mutexLock(&t->lock);
        t->st_live = live;
        t->st_claiming = claiming;
        t->st_idle_unchoked = idleUnchoked;
        t->st_bf_empty = bfEmpty;
        t->st_connecting = pending;
        if (live > t->st_peak_live) t->st_peak_live = live;
        mutexUnlock(&t->lock);
    }

out:
    if (S) {
        for (int i = 0; i < MAX_SESS; i++)
            if (S[i].active) sess_reset(t, &S[i], false);
        free(S);
    }
    free(pfd);
    free(map);
    free(buf);
    free(sbuf);
}

// Accepts peers that connect TO us and hands them to the session loop. This is
// what lets NAT'd seeds -- the majority we cannot reach with outbound TCP --
// reach us instead. Only effective when PEER_LISTEN_PORT is reachable from the
// internet (public IP, or a router port-forward / NAT-PMP / UPnP mapping).
static void acceptor_main(void *arg) {
    torrentfs *t = arg;

    while (!t->stop) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int fd = accept(t->listen_sock, (struct sockaddr *)&ca, &cl);
        if (fd < 0)
            continue;  // SO_RCVTIMEO fired or transient error; re-check t->stop
        if (t->stop) { close(fd); break; }

        // Hand it to the loop by parking it in the incoming queue; the loop
        // adopts it on its next pass.
        mutexLock(&t->lock);
        bool queued = false;
        for (int i = 0; i < INCOMING_QUEUE; i++) {
            if (t->incoming[i] < 0) { t->incoming[i] = fd; queued = true; break; }
        }
        if (queued) t->st_incoming++;
        mutexUnlock(&t->lock);
        if (!queued) close(fd);  // loop is saturated; drop it
    }
}


// Merges peers (from a tracker announce or the DHT) into the pool,
// deduplicated. Once the pool is full, a fresh peer replaces the non-busy
// entry with the most consecutive connect failures: discovery is periodic, and
// without eviction the first (stale) snapshot of the swarm would block every
// fresher one for good. Peers that ever connected (fails == 0) and peers a
// session currently holds are never evicted.
// Thread-safe: called concurrently from the tracker/DHT threads.
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
        if (slot < 0) continue;  // full of live/untried peers: drop the new one

        t->peers[slot]         = peers[i];
        t->peer_fails[slot]    = 0;
        t->peer_next_try[slot] = 0;
    }
    mutexUnlock(&t->lock);
}

// Peer discovery is periodic, not one-shot. The pool used to be a snapshot of
// the swarm taken in the first ~20 s and never refreshed; peers churn on the
// scale of minutes, so any long playback outlived every peer in it, take_peer
// then cycled a list of dead addresses forever, and the speed hit zero for
// good. Trackers only ever return currently-active swarm members, so each new
// round is what swaps the pool's dead entries for live ones (see the eviction
// in add_peers_cb).
#define DISCOVERY_INTERVAL_SECS (15 * 60)
// ...but when the swarm is down to a couple of live sessions, a fresh round is
// the only thing that can bring the speed back -- don't sit on it for 15 min.
#define DISCOVERY_STARVED_SECS  60
#define STARVED_LIVE            3

static bool tfs_starving(torrentfs *t) {
    mutexLock(&t->lock);
    bool starving = t->st_live < STARVED_LIVE;
    mutexUnlock(&t->lock);
    return starving;
}

// Sleeps until the next discovery round is due (early when starving), checking
// for shutdown every second. Returns false when it is time to stop instead.
static bool discovery_wait(torrentfs *t) {
    u64 freq  = armGetSystemTickFreq();
    u64 start = armGetSystemTick();
    while (!t->stop && !all_done(t)) {
        svcSleepThread(1000000000ULL);  // 1 s
        u64 secs = (armGetSystemTick() - start) / freq;
        if (secs >= DISCOVERY_INTERVAL_SECS) return true;
        if (secs >= DISCOVERY_STARVED_SECS && tfs_starving(t)) return true;
    }
    return false;
}

// Announces to the trackers in the background, feeding peers as they arrive, so
// torrentfs_open does not block on slow trackers before workers can start --
// then keeps re-announcing every discovery round for the life of the stream.
static void announce_worker(void *arg) {
    torrentfs *t = arg;
    char e[128];
    do
        torrent_announce_cb(&t->meta, add_peers_cb, t, e, sizeof(e));
    while (discovery_wait(t));
    t->announce_running = false;
}

static void dht_worker(void *arg) {
    torrentfs *t = arg;
    char e[128];
    do
        dht_find_peers(t->meta.info_hash, 150, 20000, add_peers_cb, t, &t->stop,
                       e, sizeof(e));
    while (discovery_wait(t));
    t->dht_running = false;
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

    // calloc leaves these at 0, which is a *valid* fd -- mark them empty.
    for (int i = 0; i < INCOMING_QUEUE; i++) t->incoming[i] = -1;
    t->listen_sock = -1;

    // A magnet's metadata fetch already announced and got a peer list. Keep it:
    // those are the peers we are about to download from.
    peer_addr seed_peers[80];
    int seed_count = 0;

    // `source` is either a magnet: URI or a path to a .torrent file.
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

    // The file the caller picked, or the largest one (the video, next to
    // subs/poster) when it did not pick -- which is all a magnet can do, since
    // its file list only exists once the metadata has landed.
    int fi = file_index;
    if (fi < 0 || fi >= t->meta.file_count) fi = torrent_largest_file(&t->meta);
    if (fi < 0) {
        snprintf(err, errlen, "torrent has no files");
        torrent_unload(&t->meta);
        free(t);
        return NULL;
    }
    t->stream_offset = t->meta.files[fi].offset;
    t->stream_size = t->meta.files[fi].length;
    // We download ONLY the pieces overlapping the target (video) file. The
    // boundary pieces are shared with adjacent files, so a little of them comes
    // along, but no unrelated file is fetched.
    t->file_first_piece = t->stream_offset / t->meta.piece_len;
    t->file_last_piece =
        (t->stream_offset + t->stream_size - 1) / t->meta.piece_len;

    t->status = calloc(1, t->meta.piece_count);
    t->blocks_per_piece =
        (int)((t->meta.piece_len + BLOCK_LEN - 1) / BLOCK_LEN);
    int64_t total_blocks = t->meta.piece_count * (int64_t)t->blocks_per_piece;
    t->block_have = calloc(1, (size_t)(total_blocks + 7) / 8);
    t->block_req  = calloc(1, (size_t)(total_blocks + 7) / 8);
    t->piece_stalled = calloc(1, (size_t)t->meta.piece_count);

    // The cache is a set of "<cache_path>.NNN" files, not one big file: FAT32
    // caps a file at 4 GB and torrents routinely exceed that. Chunks are opened
    // lazily; create chunk 0 now so a bad path fails here rather than mid-stream.
    snprintf(t->cache_base, sizeof(t->cache_base), "%s", cache_path);
    mutexInit(&t->cache_lock);
    if (!t->status || !t->block_have || !t->block_req || !t->piece_stalled ||
        !cache_chunk(t, 0)) {
        snprintf(err, errlen, "cache/status alloc failed");
        torrentfs_close(t);
        return NULL;
    }

    mutexInit(&t->lock);  // cache_lock is already initialised above, before the
                          // first cache_chunk() call

    memcpy(t->peer_id, "-SW0001-", 8);
    srand((unsigned)time(NULL));
    for (int i = 8; i < 20; i++) t->peer_id[i] = (uint8_t)(rand() % 256);

    // Announce to the trackers in the BACKGROUND (feeding peers as they arrive)
    // instead of blocking here for the slowest tracker. Workers start straight
    // away and pick up peers within a second or two of the fastest tracker.
    t->peer_count = 0;

    // Seed with the magnet's peers before the announce even starts, so the loop
    // has somewhere to connect from its very first pass instead of idling
    // through a full tracker round-trip. The announce still runs: it refreshes
    // the list and reaches trackers the metadata fetch never used.
    if (seed_count > 0) add_peers_cb(t, seed_peers, seed_count);

    t->announce_running = true;
    if (threadCreate(&t->announce_thread, announce_worker, t, NULL,
                     0x20000, 0x2C, -2) == 0) {
        t->announce_started = true;
        threadStart(&t->announce_thread);
    } else {
        t->announce_running = false;
    }

    // Start the DHT lookup (BEP 5) to discover peers beyond the trackers. It
    // feeds the pool incrementally through add_peers_cb while the workers run.
    t->dht_running = true;
    if (threadCreate(&t->dht_thread, dht_worker, t, NULL, 0x20000, 0x2C, -2) == 0) {
        t->dht_started = true;
        threadStart(&t->dht_thread);
    } else {
        t->dht_running = false;
    }

    // Listen for incoming peer connections on the port we announce to trackers.
    // accept() is given a 1 s receive timeout so acceptor threads wake to observe
    // t->stop on shutdown. Binding may fail (port busy) — incoming is best-effort.
    t->listen_sock = -1;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls >= 0) {
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        struct sockaddr_in la = {0};
        la.sin_family = AF_INET;
        la.sin_addr.s_addr = INADDR_ANY;
        la.sin_port = htons(PEER_LISTEN_PORT);
        if (bind(ls, (struct sockaddr *)&la, sizeof(la)) == 0 && listen(ls, 16) == 0)
            t->listen_sock = ls;
        else
            close(ls);
    }
    t->n_acceptors = 0;
    if (t->listen_sock >= 0) {
        for (int i = 0; i < INCOMING_ACCEPTORS; i++) {
            if (threadCreate(&t->acceptors[i], acceptor_main, t, NULL,
                             0x20000, 0x2C, -2) != 0) {
                t->acceptors[i].handle = 0;
                continue;
            }
            threadStart(&t->acceptors[i]);
            t->n_acceptors++;
        }
    }

    // One thread drives every peer session through a single poll(). Its stack
    // is bigger than the old workers' because the session table lives on it.
    if (threadCreate(&t->netloop, netloop_main, t, NULL, 0x40000, 0x2C, -2) == 0) {
        threadStart(&t->netloop);
        t->netloop_started = true;
    } else {
        t->netloop.handle = 0;
        snprintf(err, errlen, "could not start the network thread");
        torrentfs_close(t);
        return NULL;
    }

    return t;
}

void torrentfs_close(torrentfs *tfs) {
    if (!tfs) return;
    tfs->stop = true;
    // Unblock any acceptor parked in accept() WITHOUT invalidating the socket:
    // shutdown() leaves the descriptor (and its devoptab entry) alive, so a
    // concurrent accept() merely fails. close()ing it here instead frees the fd
    // handle out from under the blocked accept(), which then hands back a stale
    // descriptor -- the acceptor close()es it in peer_disconnect and newlib
    // dereferences a NULL devoptab device (data abort). The 1 s SO_RCVTIMEO on
    // the listen socket is the fallback if shutdown() doesn't wake accept().
    if (tfs->listen_sock >= 0)
        shutdown(tfs->listen_sock, SHUT_RDWR);
    // Join the acceptors before the fd is closed: they read tfs->listen_sock
    // every iteration, so it has to stay valid for as long as they can run.
    for (int i = 0; i < INCOMING_ACCEPTORS; i++) {
        if (tfs->acceptors[i].handle == 0) continue;
        threadWaitForExit(&tfs->acceptors[i]);
        threadClose(&tfs->acceptors[i]);
        tfs->acceptors[i].handle = 0;
    }
    // Safe now: no acceptor can be touching it.
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
    // The session loop owns every peer socket, so it must be joined before we
    // free anything it touches. It wakes within one poll() timeout (200 ms).
    if (tfs->netloop_started) {
        threadWaitForExit(&tfs->netloop);
        threadClose(&tfs->netloop);
        tfs->netloop_started = false;
    }
    // Any socket the acceptors queued but the loop never adopted.
    for (int i = 0; i < INCOMING_QUEUE; i++)
        if (tfs->incoming[i] >= 0) { close(tfs->incoming[i]); tfs->incoming[i] = -1; }

    for (int i = 0; i < CACHE_MAX_CHUNKS; i++)
        if (tfs->chunks[i]) { fclose(tfs->chunks[i]); tfs->chunks[i] = NULL; }
    free(tfs->status);
    free(tfs->block_have);
    free(tfs->block_req);
    free(tfs->piece_stalled);
    torrent_unload(&tfs->meta);
    free(tfs);
}

int64_t torrentfs_size(const torrentfs *tfs) {
    return tfs->stream_size;
}

void torrentfs_set_playhead(torrentfs *tfs, int64_t offset) {
    // offset is relative to the target file; map it to an absolute piece.
    int64_t abs = tfs->stream_offset + offset;
    int64_t piece = abs / tfs->meta.piece_len;
    mutexLock((Mutex *)&tfs->lock);
    ((torrentfs *)tfs)->playhead_piece = piece;
    mutexUnlock((Mutex *)&tfs->lock);
}

int64_t torrentfs_read(torrentfs *tfs, int64_t offset, char *buf, int64_t nbytes) {
    if (offset >= tfs->stream_size) return 0;      // EOF
    if (offset + nbytes > tfs->stream_size) nbytes = tfs->stream_size - offset;
    if (nbytes <= 0) return 0;

    torrentfs_set_playhead(tfs, offset);

    // Work in absolute torrent coordinates from here on.
    int64_t abs = tfs->stream_offset + offset;
    int64_t abs_total = tfs->stream_offset + tfs->stream_size;
    int64_t plen = tfs->meta.piece_len;
    int64_t first_piece = abs / plen;

    // Wait until the piece containing the first byte is available.
    while (!tfs->stop && !have_piece(tfs, first_piece))
        svcSleepThread(20000000ULL);  // 20 ms
    if (tfs->stop) return -1;

    // Extend the read across as many contiguous available pieces as the request
    // covers (short reads are allowed by the mpv stream API).
    int64_t last_byte = abs + nbytes - 1;
    int64_t last_piece = last_byte / plen;
    int64_t avail_end = (first_piece + 1) * plen;  // end of first piece
    for (int64_t pc = first_piece + 1; pc <= last_piece; pc++) {
        if (!have_piece(tfs, pc)) break;
        avail_end = (pc + 1) * plen;
    }
    if (avail_end > abs_total) avail_end = abs_total;

    int64_t can_read = avail_end - abs;
    if (can_read > nbytes) can_read = nbytes;

    mutexLock(&tfs->cache_lock);
    cache_flush(tfs);  // blocks may still be sitting in a chunk's stdio buffer
    size_t got = cache_read_upto(tfs, abs, buf, (size_t)can_read);
    mutexUnlock(&tfs->cache_lock);

    // Return what we actually read. Reporting -1 on a short read told mpv the
    // stream had failed, so it stopped reading altogether -- and since the
    // playhead only moves when mpv reads, the download froze with it.
    return (int64_t)got;
}

void torrentfs_cancel(torrentfs *tfs) {
    tfs->stop = true;
}

void torrentfs_stats(const torrentfs *tfs, int64_t *pieces_done,
                     int64_t *pieces_total, int64_t *playhead_piece) {
    mutexLock((Mutex *)&tfs->lock);
    if (pieces_done) *pieces_done = tfs->pieces_done;
    // Only the target file's pieces are ever downloaded, so report its piece
    // count as the total (otherwise "done/total" could never reach 100%).
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

// Bytes actually on disk: every block bit that is set, whatever piece it belongs
// to and whether that piece is finished. This is the honest denominator for
// "did we waste bandwidth" -- comparing bytes_recv against *completed* pieces
// undercounts, because partial pieces are real progress that survives on disk.
// A gap between this and bytes_recv is genuine duplication.
// Full state of one piece: its status, and how many of its blocks we hold vs
// have on order. The player blocks in torrentfs_read until its piece is DONE,
// so when the playhead freezes this says exactly why that piece is stuck --
// unclaimable (every block reserved), silently mid-verify, or simply not
// offered to anyone.
void torrentfs_piece_debug(const torrentfs *tfs, int64_t idx, int *status,
                           int *have, int *req, int *total) {
    mutexLock((Mutex *)&tfs->lock);
    torrentfs *t = (torrentfs *)tfs;
    int nb = 0, h = 0, r = 0;
    if (idx >= 0 && idx < t->meta.piece_count) {
        nb = (int)((torrent_piece_len(&t->meta, idx) + BLOCK_LEN - 1) / BLOCK_LEN);
        for (int b = 0; b < nb; b++) {
            if (block_get(t, idx, b)) h++;
            if (req_get(t, idx, b)) r++;
        }
        if (status) *status = t->status[idx];
    } else if (status) {
        *status = -1;
    }
    if (have) *have = h;
    if (req) *req = r;
    if (total) *total = nb;
    mutexUnlock((Mutex *)&tfs->lock);
}

int64_t torrentfs_stored_bytes(const torrentfs *tfs) {
    static const uint8_t popcount[16] = { 0, 1, 1, 2, 1, 2, 2, 3,
                                          1, 2, 2, 3, 2, 3, 3, 4 };
    mutexLock((Mutex *)&tfs->lock);
    int64_t total_blocks = tfs->meta.piece_count * (int64_t)tfs->blocks_per_piece;
    size_t nbytes        = (size_t)(total_blocks + 7) / 8;
    int64_t set          = 0;
    for (size_t i = 0; i < nbytes; i++) {
        uint8_t v = tfs->block_have[i];
        set += popcount[v & 0xF] + popcount[v >> 4];
    }
    mutexUnlock((Mutex *)&tfs->lock);
    return set * (int64_t)BLOCK_LEN;  // approximate: last block of a piece is short
}

void torrentfs_cache_stats(const torrentfs *tfs, int *wr_fail, int *rd_short,
                           int64_t *total_bytes) {
    mutexLock((Mutex *)&tfs->lock);
    if (wr_fail) *wr_fail = tfs->st_cache_wr_fail;
    if (rd_short) *rd_short = tfs->st_cache_rd_short;
    mutexUnlock((Mutex *)&tfs->lock);
    // Total size the cache file must reach: a >4 GB torrent cannot fit in one
    // file on FAT32, which is how the tail pieces become unwritable.
    if (total_bytes) *total_bytes = tfs->meta.piece_count * tfs->meta.piece_len;
}

int64_t torrentfs_cache_written(const torrentfs *tfs) {
    mutexLock((Mutex *)&tfs->lock);
    int64_t b = tfs->st_cache_written;
    mutexUnlock((Mutex *)&tfs->lock);
    return b;
}

// v1 has no latency probes; report zeros so the panel still renders.
void torrentfs_lat_stats(const torrentfs *tfs, uint32_t count[5],
                         uint64_t max_us[5]) {
    (void)tfs;
    for (int i = 0; i < 5; i++) { count[i] = 0; max_us[i] = 0; }
}

// v1 has no calm mode either.
void torrentfs_set_backlog(torrentfs *tfs, int ms) { (void)tfs; (void)ms; }
int torrentfs_calm(const torrentfs *tfs) { (void)tfs; return 0; }
void torrentfs_set_governor(int on) { (void)on; }
int torrentfs_governor(void) { return 0; }
void torrentfs_hb_ui(torrentfs *tfs) { (void)tfs; }
void torrentfs_heartbeats(const torrentfs *tfs, uint32_t age_ms[4],
                          int core[4]) {
    (void)tfs;
    for (int i = 0; i < 4; i++) { age_ms[i] = 0; core[i] = 0; }
}

void torrentfs_claim_debug(const torrentfs *tfs, int64_t *ph, int64_t *lo,
                           int64_t *hi, int *fail, int *ok, int *inflight) {
    mutexLock((Mutex *)&tfs->lock);
    if (ph) *ph = tfs->st_win_ph;
    if (lo) *lo = tfs->st_win_lo;
    if (hi) *hi = tfs->st_win_hi;
    if (fail) *fail = tfs->st_claim_fail;
    if (ok) *ok = tfs->st_claim_ok;
    if (inflight) *inflight = tfs->st_inflight;
    mutexUnlock((Mutex *)&tfs->lock);
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
