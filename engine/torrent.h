#ifndef TORRENT_H
#define TORRENT_H

#include <stddef.h>
#include <stdint.h>

#include "bencode.h"

#define MAX_TRACKERS 32
#define MAX_FILES 256

// One file within the torrent, with its byte offset in the concatenated piece
// layout (offset 0 for single-file torrents).
typedef struct {
    int64_t length;
    int64_t offset;   // absolute start within the whole-torrent byte stream
    char path[256];
} torrent_file;

typedef struct {
    char *buf;          // raw .torrent file contents (nodes point into it)
    be_node *root;
    uint8_t info_hash[20];
    char name[256];
    int64_t total_len;
    int64_t piece_len;
    int64_t piece_count;
    const uint8_t *piece_hashes;  // piece_count * 20 bytes, points into buf
    char *trackers[MAX_TRACKERS];
    int tracker_count;
    torrent_file files[MAX_FILES];
    int file_count;
} torrent_meta;

// Index of the largest file (typically the video). -1 if none.
int torrent_largest_file(const torrent_meta *t);

// Length of a given piece (the last one is usually short).
int64_t torrent_piece_len(const torrent_meta *t, int64_t index);

typedef struct {
    uint32_t ip;    // network byte order
    uint16_t port;  // host byte order
} peer_addr;

// Load and parse a .torrent file. Returns 0 on success.
int torrent_load(torrent_meta *t, const char *path, char *err, size_t errlen);

// Load from a magnet: URI — parses it, announces to its trackers to find peers,
// fetches the metadata from a peer (BEP 9), then builds the meta. Returns 0.
// Progress of a magnet's metadata fetch. It walks the swarm's peers serially
// and most of them are unreachable, so this can run for a minute; poll these to
// show it moving rather than looking hung.
extern volatile int torrent_meta_peers_tried;
extern volatile int torrent_meta_peers_total;

int torrent_load_magnet(torrent_meta *t, const char *magnet_uri,
                        char *err, size_t errlen);

// Same, but also hands back the peers the tracker gave us for the metadata
// fetch (up to `max`, count in `*out_n`). They are the same peers the download
// needs a moment later: without this the caller announces to the very same
// trackers again and starts with an empty peer list.
int torrent_load_magnet_peers(torrent_meta *t, const char *magnet_uri,
                              peer_addr *out, int max, int *out_n,
                              char *err, size_t errlen);

// Build a torrent_meta from raw metadata (the info dict fetched from peers for
// a magnet), a known info hash, and a tracker list. Takes ownership of
// `metadata` (freed by torrent_unload). Returns 0 on success.
int torrent_load_from_metadata(torrent_meta *t, uint8_t *metadata, size_t len,
                               const uint8_t info_hash[20],
                               char *const *trackers, int tracker_count,
                               char *err, size_t errlen);

void torrent_unload(torrent_meta *t);

// Optional debug logger; if set, torrent_announce reports each tracker's result
// and timing through it. Pass NULL to disable.
void torrent_set_log(void (*fn)(const char *msg));

// Announce to all trackers in parallel, merging unique peers into `peers`.
// Returns the peer count, or -1 on failure. Blocks until every tracker answers
// or times out (used by magnet metadata fetch).
int torrent_announce(const torrent_meta *t, peer_addr *peers, int max_peers,
                     char *err, size_t errlen);

// Same, but delivers peers incrementally through `cb` as each tracker responds
// (the callback must be thread-safe: trackers run on their own threads). Blocks
// until every tracker finished. Returns the number of trackers that answered,
// or -1 on failure. Lets callers start using the fastest tracker's peers without
// waiting for the slow ones.
typedef void (*torrent_peer_cb)(void *ctx, const peer_addr *peers, int n);
int torrent_announce_cb(const torrent_meta *t, torrent_peer_cb cb, void *ctx,
                        char *err, size_t errlen);

#endif
