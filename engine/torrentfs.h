#ifndef TORRENTFS_H
#define TORRENTFS_H

#include <stddef.h>
#include <stdint.h>

// Streaming torrent backend: downloads pieces on demand, prioritizing the ones
// just ahead of the current read position, and exposes a blocking byte-range
// read that waits until the requested data has arrived. Verified pieces are
// cached to a file on the SD card.
typedef struct torrentfs torrentfs;

// Open a source (either a magnet: URI or a path to a .torrent file), announce,
// allocate the cache file and start the background download workers. Returns
// NULL on failure (message in err). Streams the largest file in the torrent.
torrentfs *torrentfs_open(const char *source, const char *cache_path,
                          char *err, size_t errlen);

// Same, but streams the file at `file_index` (as ordered in torrent_meta.files).
// For a season pack "the largest file" is an arbitrary episode, so the caller
// has to be able to say which one. A negative index means the largest file.
torrentfs *torrentfs_open_file(const char *source, const char *cache_path,
                               int file_index, char *err, size_t errlen);

void torrentfs_close(torrentfs *tfs);

// Total size of the streamed file, in bytes.
int64_t torrentfs_size(const torrentfs *tfs);

// Blocking read at an absolute offset. Moves the download priority window to
// this offset, waits until at least the first requested byte is available, then
// returns as many contiguously-available bytes as possible (a short read).
// Returns bytes read, 0 at EOF, or -1 if cancelled/error.
int64_t torrentfs_read(torrentfs *tfs, int64_t offset, char *buf, int64_t nbytes);

// Move the download priority window without reading (used by seek).
void torrentfs_set_playhead(torrentfs *tfs, int64_t offset);

// Unblock any in-progress read so playback can shut down.
void torrentfs_cancel(torrentfs *tfs);

// Snapshot of progress for on-screen/debug reporting.
void torrentfs_stats(const torrentfs *tfs, int64_t *pieces_done,
                     int64_t *pieces_total, int64_t *playhead_piece);

// Number of peers found by the tracker announce.
int torrentfs_peer_count(const torrentfs *tfs);

// Rough worker diagnostics: conn_ok, conn_fail, unchoke_ok, choked, piece_ok,
// fetch_fail, sha_fail, blocks_served, interested_recv, request_recv.
// `out` must have room for 10 ints.
void torrentfs_debug_counts(const torrentfs *tfs, int out[10]);
int64_t torrentfs_bytes_recv(const torrentfs *tfs);
int torrentfs_incoming_count(const torrentfs *tfs);

// Peer sessions currently connected+handshaked, and the high-water mark. This
// is the number the poll() session loop exists to raise: the old
// thread-per-peer design was capped near 11 by libnx's 16 BSD sessions.
void torrentfs_live_peers(const torrentfs *tfs, int *live, int *peak,
                          int *connecting);

// Splits connect failures by cause: sock_fail = socket()/connect() refused
// outright (we ran out of a local resource, e.g. the BSD socket buffer pool);
// timeouts = SYN sent but the peer never answered (a genuinely dead/NAT'd peer).
void torrentfs_fail_kinds(const torrentfs *tfs, int *sock_fail, int *timeouts);

// claiming = live sessions actually downloading a piece; idle = sessions that
// are unchoked but have nothing claimed, i.e. starved by the read-ahead window.
void torrentfs_claim_stats(const torrentfs *tfs, int *claiming, int *idle);

// empty = handshaked sessions still holding an all-zero bitfield (we know of no
// piece they have, so nothing can ever be claimed from them); ok/bad = bitfield
// messages accepted / dropped because their length didn't match what we expect.
void torrentfs_bitfield_stats(const torrentfs *tfs, int *empty, int *ok, int *bad);

// The window claim_piece last searched (playhead, and the [lo,hi) range), how
// often it found nothing vs something, and how many pieces are held INFLIGHT.
void torrentfs_claim_debug(const torrentfs *tfs, int64_t *ph, int64_t *lo,
                           int64_t *hi, int *fail, int *ok, int *inflight);

// Cache-file I/O that silently failed, plus the size the cache must reach. The
// cache is a single file covering the whole torrent, so anything over 4 GB
// cannot be written on a FAT32 SD card.
void torrentfs_cache_stats(const torrentfs *tfs, int *wr_fail, int *rd_short,
                           int64_t *total_bytes);

// Bytes successfully written to the SD cache, cumulative. This is the counter
// to differentiate for a write rate: total_bytes above is the (constant) size
// the cache must reach, and stored_bytes below includes RAM partials.
int64_t torrentfs_cache_written(const torrentfs *tfs);

// How many milliseconds of playback the player has buffered ahead. The deeper
// the backlog, the fewer sessions the engine lets claim new work (a ramp from
// the full swarm below 10 s down to a single session above 30 s), because the
// whole swarm bursting at wifi line rate every window slide is what freezes
// the console (bsd/wlan pay per packet on the OS core). A stream that
// struggles to keep up stays under 10 s and is never narrowed. Defaults to 0
// (full speed) until set.
void torrentfs_set_backlog(torrentfs *tfs, int ms);

// Sessions currently allowed to claim work (the calm-mode budget); equals the
// session cap when unrestricted. For the debug panel.
int torrentfs_calm(const torrentfs *tfs);

// Fill-rate governor (an Options toggle, on by default): when the backlog is
// deep, pause claiming while the measured download rate exceeds a
// backlog-tied target (unlimited under 10 s of backlog, tapering to 1.5 MB/s
// above 25 s). Session count alone cannot bound the bursts -- one fast peer
// saturates the wifi on its own. Global, safe to flip mid-playback.
void torrentfs_set_governor(int on);
int  torrentfs_governor(void);

// RAM streaming mode (an Options toggle, on by default): keep verified pieces
// in a bounded RAM window instead of writing them to the SD card. Completing a
// piece then never bursts the FAT filesystem service on the OS core, which is
// what stutters playback at each piece boundary -- the bigger the piece, the
// bigger the stall. The trade: nothing is persisted, and seeking back beyond
// the window re-downloads. Read once at torrentfs_open time (a live torrent
// keeps the mode it opened with), so set it before opening. Needs a full-RAM
// launch: the window plus mpv's own buffers will not fit an applet's heap.
void torrentfs_set_ram_stream(int on);
int  torrentfs_ram_stream(void);

// Thread heartbeats, indexed netloop/writer/reader/ui: how long ago (ms) each
// probe thread last ran, and the CPU core it last ran on. The freeze episodes
// stall several threads at once without any syscall being slow -- these say
// which threads stop being scheduled and where the survivors run. The UI
// thread must call torrentfs_hb_ui() itself (once per frame).
void torrentfs_hb_ui(torrentfs *tfs);
void torrentfs_heartbeats(const torrentfs *tfs, uint32_t age_ms[4],
                          int core[4]);

// Syscall latency probes, indexed poll/recv/send/sd-write/sd-read: cumulative
// call counts, and the worst single-call duration (µs) since the last read
// (reading clears the peaks). When the console freezes, the class whose peak
// explodes names the OS service that stalled: bsd for the first three, fs for
// the last two. An idle poll legitimately reaches ~200 ms (its timeout).
void torrentfs_lat_stats(const torrentfs *tfs, uint32_t count[5],
                         uint64_t max_us[5]);

// Bytes actually held on disk, counting partial pieces. Compare against
// torrentfs_bytes_recv(): the difference is bandwidth that was genuinely
// duplicated, as opposed to progress sitting in unfinished pieces.
int64_t torrentfs_stored_bytes(const torrentfs *tfs);

// State of a single piece: status (PIECE_NEEDED/INFLIGHT/DONE/VERIFYING), and
// how many of its blocks are held vs on order. Use it on the playhead piece:
// that is the one the player blocks on, so if the playhead freezes this shows
// why that exact piece never lands.
void torrentfs_piece_debug(const torrentfs *tfs, int64_t idx, int *status,
                           int *have, int *req, int *total);
int torrentfs_piece_done(const torrentfs *tfs, int64_t idx);
int64_t torrentfs_piece_len(const torrentfs *tfs);

// The torrent's name, for display. Never NULL.
const char *torrentfs_name(const torrentfs *tfs);

// Last piece-fetch failure reason, for diagnostics.
void torrentfs_last_err(const torrentfs *tfs, char *buf, size_t len);

#endif
