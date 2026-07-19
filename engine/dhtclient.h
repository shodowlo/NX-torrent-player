#ifndef DHTCLIENT_H
#define DHTCLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "torrent.h"  // peer_addr

// Peer discovery over the mainline DHT (BEP 5), implemented on top of the
// battle-tested jech/dht library (source/dht.c). Peers are delivered
// incrementally through `cb` as the lookup finds them.
//
// Runs for at most `budget_ms`, or stops early once about `target_peers` peers
// have been delivered. `cancel` (may be NULL) is polled to stop early. Returns
// the number of peers delivered, or -1 on failure (message in err).
typedef void (*dht_peer_cb)(void *ctx, const peer_addr *peers, int n);

int dht_find_peers(const uint8_t info_hash[20], int target_peers, int budget_ms,
                   dht_peer_cb cb, void *ctx, const volatile bool *cancel,
                   char *err, size_t errlen);

// Optional debug logger; if set, the lookup reports progress through it.
void dht_set_log(void (*fn)(const char *msg));

#endif
