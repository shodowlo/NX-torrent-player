#ifndef MAGNET_H
#define MAGNET_H

#include <stddef.h>
#include <stdint.h>

#include "torrent.h"  // for MAX_TRACKERS

// Parsed magnet link: the info hash plus any embedded trackers.
typedef struct {
    uint8_t info_hash[20];
    char name[256];
    char *trackers[MAX_TRACKERS];
    int tracker_count;
} magnet_info;

// Parse a magnet: URI. Returns 0 on success. Frees with magnet_free.
int magnet_parse(const char *uri, magnet_info *m, char *err, size_t errlen);
void magnet_free(magnet_info *m);

#endif
