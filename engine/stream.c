#include "stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mpv/stream_cb.h>

#include "torrentfs.h"

// Per-open-stream state: the shared torrent store plus this stream's cursor.
typedef struct {
    torrentfs *tfs;
    int64_t pos;
} stream_ctx;

static int64_t read_cb(void *cookie, char *buf, uint64_t nbytes) {
    stream_ctx *s = cookie;
    int64_t n = torrentfs_read(s->tfs, s->pos, buf, (int64_t)nbytes);
    if (n > 0) s->pos += n;
    return n;  // 0 = EOF, -1 = error/cancel
}

static int64_t seek_cb(void *cookie, int64_t offset) {
    stream_ctx *s = cookie;
    s->pos = offset;
    torrentfs_set_playhead(s->tfs, offset);  // reprioritize downloads here
    return offset;
}

static int64_t size_cb(void *cookie) {
    stream_ctx *s = cookie;
    int64_t sz = torrentfs_size(s->tfs);
    return sz > 0 ? sz : MPV_ERROR_UNSUPPORTED;
}

static void close_cb(void *cookie) {
    free(cookie);  // the torrentfs itself is owned by main, not the stream
}

static int open_cb(void *user_data, char *uri, mpv_stream_cb_info *info) {
    (void)uri;
    stream_ctx *s = calloc(1, sizeof(*s));
    if (!s) return MPV_ERROR_LOADING_FAILED;
    s->tfs = user_data;
    s->pos = 0;

    info->cookie = s;
    info->read_fn = read_cb;
    info->seek_fn = seek_cb;
    info->size_fn = size_cb;
    info->close_fn = close_cb;
    return 0;
}

int stream_register(mpv_handle *mpv, torrentfs *tfs) {
    return mpv_stream_cb_add_ro(mpv, "torrent", tfs, open_cb);
}
