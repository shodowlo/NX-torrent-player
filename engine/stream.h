#ifndef STREAM_H
#define STREAM_H

#include <mpv/client.h>

#include "torrentfs.h"

// Registers a custom mpv protocol so `loadfile torrent://...` reads through our
// callbacks, backed by the streaming torrent store `tfs`. mpv issues blocking
// read/seek calls; the torrentfs layer downloads on demand around the playhead.
//
// Returns 0 on success. `tfs` must outlive the mpv instance.
int stream_register(mpv_handle *mpv, torrentfs *tfs);

#endif
