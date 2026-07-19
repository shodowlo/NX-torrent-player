#include "torrent.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <switch.h>
#include <curl/curl.h>
#include <mbedtls/sha1.h>

#include "udp_tracker.h"
#include "magnet.h"
#include "peer.h"

static void set_err(char *err, size_t errlen, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s", msg);
}

static void (*s_log_fn)(const char *) = NULL;

void torrent_set_log(void (*fn)(const char *)) { s_log_fn = fn; }

static void tlog(const char *fmt, ...) {
    if (!s_log_fn) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s_log_fn(buf);
}

// Well-known public UDP trackers, appended to every torrent so we discover far
// more peers than the torrent's own (often stale) tracker list provides. UDP
// only: HTTP(S) trackers need a CA bundle we don't ship. add_tracker dedups.
static const char *DEFAULT_TRACKERS[] = {
    "udp://tracker.opentrackr.org:1337/announce",
    "udp://open.stealth.si:80/announce",
    "udp://tracker.torrent.eu.org:451/announce",
    "udp://exodus.desync.com:6969/announce",
    "udp://open.demonii.com:1337/announce",
    "udp://tracker.openbittorrent.com:6969/announce",
    "udp://explodie.org:6969/announce",
};

static void add_tracker(torrent_meta *t, const char *url, size_t len);

static void add_default_trackers(torrent_meta *t) {
    for (size_t i = 0; i < sizeof(DEFAULT_TRACKERS) / sizeof(*DEFAULT_TRACKERS); i++)
        add_tracker(t, DEFAULT_TRACKERS[i], strlen(DEFAULT_TRACKERS[i]));
}

static void add_tracker(torrent_meta *t, const char *url, size_t len) {
    if (t->tracker_count >= MAX_TRACKERS) return;
    int is_http = len > 4 && strncmp(url, "http", 4) == 0;
    int is_udp = len > 6 && strncmp(url, "udp://", 6) == 0;
    if (!is_http && !is_udp) return;
    for (int i = 0; i < t->tracker_count; i++)
        if (strlen(t->trackers[i]) == len && strncmp(t->trackers[i], url, len) == 0)
            return;
    char *copy = malloc(len + 1);
    if (!copy) return;
    memcpy(copy, url, len);
    copy[len] = '\0';
    t->trackers[t->tracker_count++] = copy;
}

// Fills name/piece_len/piece_count/piece_hashes/files/total_len from an info
// dict. Shared by .torrent loading and magnet metadata. piece_hashes points
// into the node's backing buffer, which must stay alive (t->buf / t->root).
static int parse_info_fields(torrent_meta *t, be_node *info, char *err, size_t errlen) {
    be_node *name = be_dict_get(info, "name");
    if (name && name->type == BE_STR) {
        size_t n = name->str.len < sizeof(t->name) - 1 ? name->str.len : sizeof(t->name) - 1;
        memcpy(t->name, name->str.ptr, n);
        t->name[n] = '\0';
    }

    be_node *plen = be_dict_get(info, "piece length");
    if (!plen || plen->type != BE_INT || plen->i <= 0) {
        set_err(err, errlen, "missing 'piece length'");
        return -1;
    }
    t->piece_len = plen->i;

    be_node *pieces = be_dict_get(info, "pieces");
    if (!pieces || pieces->type != BE_STR || pieces->str.len % 20 != 0) {
        set_err(err, errlen, "invalid 'pieces'");
        return -1;
    }
    t->piece_count = pieces->str.len / 20;
    t->piece_hashes = (const uint8_t *)pieces->str.ptr;

    be_node *length = be_dict_get(info, "length");
    if (length && length->type == BE_INT) {
        // Single-file torrent: synthesize one file entry so callers can treat
        // single- and multi-file torrents uniformly.
        t->total_len = length->i;
        t->file_count = 1;
        t->files[0].length = length->i;
        t->files[0].offset = 0;
        snprintf(t->files[0].path, sizeof(t->files[0].path), "%s", t->name);
    } else {
        be_node *files = be_dict_get(info, "files");
        if (!files || files->type != BE_LIST) {
            set_err(err, errlen, "ni 'length' ni 'files'");
            return -1;
        }
        int64_t off = 0;
        for (size_t i = 0; i < files->list.count && t->file_count < MAX_FILES; i++) {
            be_node *flen = be_dict_get(files->list.items[i], "length");
            if (!flen || flen->type != BE_INT) continue;

            torrent_file *tf = &t->files[t->file_count++];
            tf->length = flen->i;
            tf->offset = off;
            off += flen->i;
            t->total_len += flen->i;

            // Join the path components with '/'.
            be_node *plist = be_dict_get(files->list.items[i], "path");
            size_t used = 0;
            tf->path[0] = '\0';
            if (plist && plist->type == BE_LIST) {
                for (size_t j = 0; j < plist->list.count; j++) {
                    be_node *comp = plist->list.items[j];
                    if (comp->type != BE_STR) continue;
                    int n = snprintf(tf->path + used, sizeof(tf->path) - used,
                                     "%s%.*s", j ? "/" : "",
                                     (int)comp->str.len, comp->str.ptr);
                    if (n < 0) break;
                    used += (size_t)n;
                    if (used >= sizeof(tf->path)) break;
                }
            }
        }
    }
    return 0;
}

// Build a torrent_meta from raw metadata bytes (the info dict, as fetched from
// peers for a magnet link) plus a known info hash and tracker list. Takes
// ownership of `metadata` (stored as t->buf, freed by torrent_unload).
int torrent_load_from_metadata(torrent_meta *t, uint8_t *metadata, size_t len,
                               const uint8_t info_hash[20],
                               char *const *trackers, int tracker_count,
                               char *err, size_t errlen) {
    memset(t, 0, sizeof(*t));
    t->buf = (char *)metadata;
    t->root = be_parse(t->buf, len);
    if (!t->root || t->root->type != BE_DICT) {
        torrent_unload(t);
        set_err(err, errlen, "invalid bencode metadata");
        return -1;
    }

    memcpy(t->info_hash, info_hash, 20);

    // The metadata IS the info dict, so parse fields directly from the root.
    if (parse_info_fields(t, t->root, err, errlen) != 0) {
        torrent_unload(t);
        return -1;
    }

    for (int i = 0; i < tracker_count; i++)
        add_tracker(t, trackers[i], strlen(trackers[i]));
    add_default_trackers(t);

    if (t->tracker_count == 0) {
        torrent_unload(t);
        set_err(err, errlen, "magnet sans tracker utilisable");
        return -1;
    }
    return 0;
}

// Progress of the magnet metadata fetch, for the UI to poll. Plain ints written
// from the loader thread and read from the UI thread: a torn read only shows a
// stale count for one frame, which is not worth a lock.
volatile int torrent_meta_peers_tried = 0;
volatile int torrent_meta_peers_total = 0;

// How many peers we ask for the metadata at once.
//
// This used to be one at a time, and that is why opening a magnet took minutes:
// most of a swarm is unreachable, and every dead peer costs a full connect
// timeout before the next one is even tried. The peers that DO answer reply in
// well under a second, so the whole wait was spent on the ones that never
// would.
//
// The ceiling is libnx: each blocking socket call holds one of 16 BSD sessions
// (NX_SESSION_MGR_MAX_SESSIONS), and a worker sits inside connect()/recv() the
// whole time it holds a peer. 8 leaves room for the app's own HTTP (a poster
// fetch may still be in flight) without ever approaching the cap.
#define META_WORKERS 8

typedef struct {
    peer_addr *peers;
    int n;
    const uint8_t *info_hash;
    const uint8_t *peer_id;

    Mutex lock;
    int next;             // next peer to hand out
    volatile bool done;   // someone has it; the others can stop
    uint8_t *metadata;    // the winner's, owned by the caller afterwards
    size_t meta_len;
} meta_fetch;

static void meta_worker(void *arg) {
    meta_fetch *c = (meta_fetch *)arg;

    for (;;) {
        if (c->done) return;

        mutexLock(&c->lock);
        int i = c->next < c->n ? c->next++ : -1;
        // Published from inside the lock so the UI's counter only ever moves
        // forward, however many workers are running.
        torrent_meta_peers_tried = c->next;
        mutexUnlock(&c->lock);
        if (i < 0) return;  // list exhausted

        uint8_t *md = NULL;
        size_t len = 0;
        char e[128];
        if (peer_fetch_metadata(c->peers[i], c->info_hash, c->peer_id, &md, &len,
                                e, sizeof(e)) != 0)
            continue;

        mutexLock(&c->lock);
        bool first = c->metadata == NULL;
        if (first) {
            c->metadata = md;
            c->meta_len = len;
        }
        c->done = true;
        mutexUnlock(&c->lock);

        if (!first) free(md);  // lost the race; another worker was quicker
        return;
    }
}

int torrent_load_magnet(torrent_meta *t, const char *magnet_uri,
                        char *err, size_t errlen) {
    return torrent_load_magnet_peers(t, magnet_uri, NULL, 0, NULL, err, errlen);
}

int torrent_load_magnet_peers(torrent_meta *t, const char *magnet_uri,
                              peer_addr *out, int max, int *out_n,
                              char *err, size_t errlen) {
    if (out_n) *out_n = 0;
    memset(t, 0, sizeof(*t));

    magnet_info m;
    if (magnet_parse(magnet_uri, &m, err, errlen) != 0)
        return -1;
    if (m.tracker_count == 0) {
        magnet_free(&m);
        set_err(err, errlen, "magnet has no tracker (DHT bootstrap not supported yet)");
        return -1;
    }

    // Announce with a temporary stub (info_hash + borrowed trackers) to find
    // peers, before we know piece counts or size.
    torrent_meta stub;
    memset(&stub, 0, sizeof(stub));
    memcpy(stub.info_hash, m.info_hash, 20);
    for (int i = 0; i < m.tracker_count; i++) stub.trackers[i] = m.trackers[i];
    stub.tracker_count = m.tracker_count;
    stub.total_len = 0;

    peer_addr peers[80];
    int n = torrent_announce(&stub, peers, 80, err, errlen);
    // stub trackers are borrowed from the magnet; do not unload it.
    if (n <= 0) {
        magnet_free(&m);
        set_err(err, errlen, "no peer to fetch metadata from");
        return -1;
    }

    uint8_t peer_id[20];
    memcpy(peer_id, "-SW0001-", 8);
    srand((unsigned)time(NULL));
    for (int i = 8; i < 20; i++) peer_id[i] = (uint8_t)(rand() % 256);

    // Ask several peers at once for the metadata (BEP 9); the first to answer
    // wins and the rest stop. Progress is published for the UI to poll.
    torrent_meta_peers_total = n;
    torrent_meta_peers_tried = 0;

    meta_fetch fetch;
    memset(&fetch, 0, sizeof(fetch));
    mutexInit(&fetch.lock);
    fetch.peers     = peers;
    fetch.n         = n;
    fetch.info_hash = m.info_hash;
    fetch.peer_id   = peer_id;

    int nw = n < META_WORKERS ? n : META_WORKERS;
    Thread workers[META_WORKERS];
    int started = 0;
    for (int i = 0; i < nw; i++) {
        if (threadCreate(&workers[started], meta_worker, &fetch, NULL, 0x10000,
                         0x2C, -2) != 0)
            break;
        if (threadStart(&workers[started]) != 0) {
            threadClose(&workers[started]);
            break;
        }
        started++;
    }
    // No worker could start: still fetch it, just on this thread. Slow beats
    // "no peer provided the metadata" when the swarm was fine.
    if (started == 0)
        meta_worker(&fetch);

    for (int i = 0; i < started; i++) {
        threadWaitForExit(&workers[i]);
        threadClose(&workers[i]);
    }

    uint8_t *metadata = fetch.metadata;
    size_t meta_len   = fetch.meta_len;
    if (!metadata) {
        magnet_free(&m);
        set_err(err, errlen, "no peer provided the metadata");
        return -1;
    }

    // Hand the peers on: the caller is about to want exactly these, and asking
    // the trackers again for the same list costs a round-trip with nothing to
    // download in the meantime.
    if (out && max > 0 && out_n) {
        int c = n < max ? n : max;
        memcpy(out, peers, (size_t)c * sizeof(peers[0]));
        *out_n = c;
    }

    int rc = torrent_load_from_metadata(t, metadata, meta_len, m.info_hash,
                                        m.trackers, m.tracker_count, err, errlen);
    magnet_free(&m);
    return rc;
}

int torrent_load(torrent_meta *t, const char *path, char *err, size_t errlen) {
    memset(t, 0, sizeof(*t));

    FILE *f = fopen(path, "rb");
    if (!f) { set_err(err, errlen, "file not found"); return -1; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 16 * 1024 * 1024) {
        fclose(f);
        set_err(err, errlen, "invalid file size");
        return -1;
    }
    t->buf = malloc(fsize);
    if (!t->buf || fread(t->buf, 1, fsize, f) != (size_t)fsize) {
        fclose(f);
        torrent_unload(t);
        set_err(err, errlen, "read error");
        return -1;
    }
    fclose(f);

    t->root = be_parse(t->buf, fsize);
    if (!t->root) {
        torrent_unload(t);
        set_err(err, errlen, "invalid bencode");
        return -1;
    }

    be_node *info = be_dict_get(t->root, "info");
    if (!info || info->type != BE_DICT) {
        torrent_unload(t);
        set_err(err, errlen, "missing 'info' dict");
        return -1;
    }

    // info_hash = SHA-1 of the info dict exactly as it appears in the file
    mbedtls_sha1((const unsigned char *)info->raw, info->rawlen, t->info_hash);

    if (parse_info_fields(t, info, err, errlen) != 0) {
        torrent_unload(t);
        return -1;
    }

    // announce-list (tiers of trackers), then plain announce as fallback
    be_node *alist = be_dict_get(t->root, "announce-list");
    if (alist && alist->type == BE_LIST) {
        for (size_t i = 0; i < alist->list.count; i++) {
            be_node *tier = alist->list.items[i];
            if (tier->type != BE_LIST) continue;
            for (size_t j = 0; j < tier->list.count; j++) {
                be_node *url = tier->list.items[j];
                if (url->type == BE_STR)
                    add_tracker(t, url->str.ptr, url->str.len);
            }
        }
    }
    be_node *announce = be_dict_get(t->root, "announce");
    if (announce && announce->type == BE_STR)
        add_tracker(t, announce->str.ptr, announce->str.len);

    add_default_trackers(t);

    if (t->tracker_count == 0) {
        torrent_unload(t);
        set_err(err, errlen, "no usable tracker");
        return -1;
    }
    return 0;
}

int torrent_largest_file(const torrent_meta *t) {
    int best = -1;
    int64_t best_len = -1;
    for (int i = 0; i < t->file_count; i++) {
        if (t->files[i].length > best_len) {
            best_len = t->files[i].length;
            best = i;
        }
    }
    return best;
}

int64_t torrent_piece_len(const torrent_meta *t, int64_t index) {
    if (index == t->piece_count - 1) {
        int64_t rem = t->total_len % t->piece_len;
        if (rem) return rem;
    }
    return t->piece_len;
}

void torrent_unload(torrent_meta *t) {
    for (int i = 0; i < t->tracker_count; i++)
        free(t->trackers[i]);
    be_free(t->root);
    free(t->buf);
    memset(t, 0, sizeof(*t));
}

//---------------------------------------------------------------------------
// Tracker announce
//---------------------------------------------------------------------------

typedef struct {
    char *data;
    size_t len;
} membuf;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    membuf *m = userdata;
    size_t add = size * nmemb;
    if (m->len + add > 4 * 1024 * 1024) return 0;  // sanity cap
    char *grown = realloc(m->data, m->len + add + 1);
    if (!grown) return 0;
    m->data = grown;
    memcpy(m->data + m->len, ptr, add);
    m->len += add;
    m->data[m->len] = '\0';
    return add;
}

static void urlencode_bytes(const uint8_t *in, size_t len, char *out) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        uint8_t c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            *out++ = c;
        } else {
            *out++ = '%';
            *out++ = hex[c >> 4];
            *out++ = hex[c & 0xF];
        }
    }
    *out = '\0';
}

static int parse_peers(const be_node *resp, peer_addr *peers, int max_peers) {
    be_node *plist = be_dict_get(resp, "peers");
    if (!plist) return 0;
    int count = 0;

    if (plist->type == BE_STR) {
        // Compact format: 6 bytes per peer (4 IP + 2 port, network order)
        size_t n = plist->str.len / 6;
        for (size_t i = 0; i < n && count < max_peers; i++) {
            const uint8_t *p = (const uint8_t *)plist->str.ptr + i * 6;
            memcpy(&peers[count].ip, p, 4);
            peers[count].port = (uint16_t)((p[4] << 8) | p[5]);
            if (peers[count].port) count++;
        }
    } else if (plist->type == BE_LIST) {
        // Dict format: list of {"ip": str, "port": int}
        for (size_t i = 0; i < plist->list.count && count < max_peers; i++) {
            be_node *ip = be_dict_get(plist->list.items[i], "ip");
            be_node *port = be_dict_get(plist->list.items[i], "port");
            if (!ip || ip->type != BE_STR || !port || port->type != BE_INT) continue;
            char ipstr[64];
            size_t n = ip->str.len < sizeof(ipstr) - 1 ? ip->str.len : sizeof(ipstr) - 1;
            memcpy(ipstr, ip->str.ptr, n);
            ipstr[n] = '\0';
            unsigned a, b, c, d;
            if (sscanf(ipstr, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) continue;
            uint8_t raw[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
            memcpy(&peers[count].ip, raw, 4);
            peers[count].port = (uint16_t)port->i;
            if (peers[count].port) count++;
        }
    }
    return count;
}

// Merges peers from `src` (count `sn`) into `dst` (current size *dn, capacity
// max), skipping duplicates by ip+port. Returns how many were newly added.
static int merge_peers(peer_addr *dst, int *dn, int max,
                       const peer_addr *src, int sn) {
    int added = 0;
    for (int i = 0; i < sn && *dn < max; i++) {
        bool dup = false;
        for (int j = 0; j < *dn; j++)
            if (dst[j].ip == src[i].ip && dst[j].port == src[i].port) {
                dup = true;
                break;
            }
        if (dup) continue;
        dst[(*dn)++] = src[i];
        added++;
    }
    return added;
}

// Announces to a single HTTP(S) tracker, filling `out` (up to max). Returns the
// peer count, or -1 on failure (message in err).
static int announce_http(const char *tracker, const char *hash_enc,
                         const char *peer_id_enc, int64_t left,
                         peer_addr *out, int max, char *err, size_t errlen) {
    char url[1024];
    snprintf(url, sizeof(url),
             "%s%cinfo_hash=%s&peer_id=%s&port=6881&uploaded=0&downloaded=0"
             "&left=%lld&compact=1&event=started&numwant=%d",
             tracker, strchr(tracker, '?') ? '&' : '?',
             hash_enc, peer_id_enc, (long long)left, max);

    membuf resp = {0};
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);  // bound announce/close time
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // Pas de bundle CA sur Switch : on désactive la vérif TLS pour l'instant
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SwitchTorrent/0.1");
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        snprintf(err, errlen, "http: %s", curl_easy_strerror(rc));
        free(resp.data);
        return -1;
    }

    be_node *root = be_parse(resp.data, resp.len);
    if (!root) {
        set_err(err, errlen, "invalid tracker response");
        free(resp.data);
        return -1;
    }
    be_node *failure = be_dict_get(root, "failure reason");
    if (failure && failure->type == BE_STR) {
        snprintf(err, errlen, "tracker: %.*s",
                 (int)(failure->str.len < 128 ? failure->str.len : 128),
                 failure->str.ptr);
        be_free(root);
        free(resp.data);
        return -1;
    }

    int count = parse_peers(root, out, max);
    be_free(root);
    free(resp.data);
    return count;
}

// One tracker announce, run on its own thread. Each job holds its own result
// buffer so no locking is needed; results are merged after all threads join.
#define AJOB_PEERS 128

typedef struct {
    const char *tracker;
    const uint8_t *info_hash;
    uint8_t peer_id[20];
    char hash_enc[61];
    char peer_id_enc[61];
    int64_t left;

    peer_addr peers[AJOB_PEERS];
    int count;
    double secs;

    torrent_peer_cb cb;   // delivered from this thread as soon as peers arrive
    void *cb_ctx;

    Thread thread;
    bool has_thread;
} ajob;

static void announce_thread(void *arg) {
    ajob *j = arg;
    char terr[128];
    u64 freq = armGetSystemTickFreq();
    u64 t0 = armGetSystemTick();

    if (strncmp(j->tracker, "udp://", 6) == 0)
        j->count = udp_announce(j->tracker, j->info_hash, j->peer_id, j->left,
                                j->peers, AJOB_PEERS, terr, sizeof(terr));
    else
        j->count = announce_http(j->tracker, j->hash_enc, j->peer_id_enc, j->left,
                                 j->peers, AJOB_PEERS, terr, sizeof(terr));

    j->secs = (double)(armGetSystemTick() - t0) / freq;

    // Hand the peers over immediately so callers don't wait for slow trackers.
    if (j->count > 0 && j->cb) j->cb(j->cb_ctx, j->peers, j->count);
}

int torrent_announce_cb(const torrent_meta *t, torrent_peer_cb cb, void *ctx,
                        char *err, size_t errlen) {
    char hash_enc[61], peer_id_enc[61];
    uint8_t peer_id[20];

    memcpy(peer_id, "-SW0001-", 8);
    srand((unsigned)time(NULL));
    for (int i = 8; i < 20; i++) peer_id[i] = (uint8_t)(rand() % 256);

    urlencode_bytes(t->info_hash, 20, hash_enc);
    urlencode_bytes(peer_id, 20, peer_id_enc);

    // Announce to every tracker in PARALLEL, delivering peers via the callback
    // the moment each tracker answers (so the fastest one unblocks downloading).
    ajob *jobs = calloc(t->tracker_count, sizeof(*jobs));
    if (!jobs) { set_err(err, errlen, "out of memory (announce)"); return -1; }

    for (int i = 0; i < t->tracker_count; i++) {
        ajob *j = &jobs[i];
        j->tracker = t->trackers[i];
        j->info_hash = t->info_hash;
        memcpy(j->peer_id, peer_id, 20);
        memcpy(j->hash_enc, hash_enc, sizeof(hash_enc));
        memcpy(j->peer_id_enc, peer_id_enc, sizeof(peer_id_enc));
        j->left = t->total_len;
        j->cb = cb;
        j->cb_ctx = ctx;

        if (threadCreate(&j->thread, announce_thread, j, NULL, 0x8000, 0x2C, -2) == 0) {
            j->has_thread = true;
            threadStart(&j->thread);
        } else {
            announce_thread(j);  // fall back to inline if the thread won't start
        }
    }

    int answered = 0;
    for (int i = 0; i < t->tracker_count; i++) {
        ajob *j = &jobs[i];
        if (j->has_thread) {
            threadWaitForExit(&j->thread);
            threadClose(&j->thread);
        }
        if (j->count > 0) answered++;
        tlog("tracker %.1fs (%d) %.60s", j->secs, j->count, j->tracker);
    }

    free(jobs);
    return answered;
}

// Collector so the array-based torrent_announce reuses the callback machinery.
typedef struct {
    peer_addr *peers;
    int total;
    int max;
    Mutex lock;
} collector;

static void collect_cb(void *ctx, const peer_addr *peers, int n) {
    collector *c = ctx;
    mutexLock(&c->lock);
    merge_peers(c->peers, &c->total, c->max, peers, n);
    mutexUnlock(&c->lock);
}

int torrent_announce(const torrent_meta *t, peer_addr *peers, int max_peers,
                     char *err, size_t errlen) {
    collector c = { peers, 0, max_peers };
    mutexInit(&c.lock);
    torrent_announce_cb(t, collect_cb, &c, err, errlen);
    if (c.total == 0) { set_err(err, errlen, "no peer from the trackers"); return -1; }
    return c.total;
}
