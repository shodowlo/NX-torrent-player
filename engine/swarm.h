#ifndef SWARM_H
#define SWARM_H

#include <stdint.h>
#include <stdio.h>

#include "torrent.h"

// Download the first `pieces_wanted` pieces using up to `max_workers` parallel
// peer connections. Verified pieces are written to `out` at their correct
// offset. Prints live progress. Returns the number of pieces obtained.
int64_t swarm_download(const torrent_meta *t, peer_addr *peers, int peer_count,
                       const uint8_t peer_id[20], FILE *out,
                       int64_t pieces_wanted, int max_workers);

#endif
