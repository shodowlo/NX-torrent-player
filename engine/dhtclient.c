#include "dhtclient.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <switch.h>
#include <mbedtls/sha1.h>

#include "dht.h"  // jech/dht (no include guard; needs the headers above first)

//-----------------------------------------------------------------------------
// Logging
//-----------------------------------------------------------------------------

static void (*s_log_fn)(const char *) = NULL;

void dht_set_log(void (*fn)(const char *)) { s_log_fn = fn; }

static void dlog(const char *fmt, ...) {
    if (!s_log_fn) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s_log_fn(buf);
}

//-----------------------------------------------------------------------------
// Callbacks required by jech/dht
//-----------------------------------------------------------------------------

int dht_blacklisted(const struct sockaddr *sa, int salen) {
    (void)sa; (void)salen;
    return 0;
}

// Deterministic hash over up to three inputs, used by the DHT for tokens and
// node ids. SHA-1 gives 20 bytes; copy/repeat to fill hash_size.
void dht_hash(void *hash_return, int hash_size,
              const void *v1, int len1,
              const void *v2, int len2,
              const void *v3, int len3) {
    mbedtls_sha1_context ctx;
    mbedtls_sha1_init(&ctx);
    mbedtls_sha1_starts_ret(&ctx);
    if (v1 && len1 > 0) mbedtls_sha1_update_ret(&ctx, v1, len1);
    if (v2 && len2 > 0) mbedtls_sha1_update_ret(&ctx, v2, len2);
    if (v3 && len3 > 0) mbedtls_sha1_update_ret(&ctx, v3, len3);
    uint8_t digest[20];
    mbedtls_sha1_finish_ret(&ctx, digest);
    mbedtls_sha1_free(&ctx);

    uint8_t *out = hash_return;
    for (int i = 0; i < hash_size; i++) out[i] = digest[i % 20];
}

int dht_random_bytes(void *buf, size_t size) {
    randomGet(buf, size);
    return (int)size;
}

//-----------------------------------------------------------------------------
// Lookup
//-----------------------------------------------------------------------------

static const char *BOOTSTRAP[] = {
    "router.bittorrent.com",
    "router.utorrent.com",
    "dht.transmission.com",
    "dht.libtorrent.org",
};
#define BOOTSTRAP_PORT "6881"

typedef struct {
    dht_peer_cb cb;
    void *ctx;
    int delivered;
} dht_ctx;

// jech/dht calls this when a search yields results or finishes.
static void on_dht_event(void *closure, int event,
                         const unsigned char *info_hash,
                         const void *data, size_t data_len) {
    (void)info_hash;
    dht_ctx *dc = closure;
    if (event != DHT_EVENT_VALUES) return;  // ignore IPv6 + search-done here

    // data is a packed array of 6-byte compact peers (4B IP + 2B port).
    int n = (int)(data_len / 6);
    if (n <= 0) return;
    const uint8_t *b = data;

    peer_addr peers[128];
    int np = 0;
    for (int i = 0; i < n && np < 128; i++) {
        const uint8_t *e = b + i * 6;
        memcpy(&peers[np].ip, e, 4);                       // network byte order
        peers[np].port = (uint16_t)((e[4] << 8) | e[5]);   // host byte order
        if (peers[np].port) np++;
    }
    if (np > 0 && dc->cb) {
        dc->cb(dc->ctx, peers, np);
        dc->delivered += np;
    }
}

int dht_find_peers(const uint8_t info_hash[20], int target_peers, int budget_ms,
                   dht_peer_cb cb, void *ctx, const volatile bool *cancel,
                   char *err, size_t errlen) {
    dlog("DHT demarre (jech)");

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        if (err) snprintf(err, errlen, "DHT socket failed");
        return -1;
    }
    struct sockaddr_in me = {0};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = INADDR_ANY;
    me.sin_port = 0;  // ephemeral; DHT replies come back to the source port
    bind(s, (struct sockaddr *)&me, sizeof(me));

    // Switch's select() does not report UDP readability, so we use a blocking
    // recvfrom with a receive timeout instead (the pattern that works here).
    struct timeval rcvto = { 1, 0 };  // 1s
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof(rcvto));

    uint8_t node_id[20];
    randomGet(node_id, sizeof(node_id));

    if (dht_init(s, -1, node_id, NULL) < 0) {
        if (err) snprintf(err, errlen, "dht_init failed");
        close(s);
        return -1;
    }

    // Bootstrap: ping the well-known routers so the routing table fills up.
    int booted = 0;
    for (size_t i = 0; i < sizeof(BOOTSTRAP) / sizeof(*BOOTSTRAP); i++) {
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(BOOTSTRAP[i], BOOTSTRAP_PORT, &hints, &res) != 0 || !res)
            continue;
        dht_ping_node(res->ai_addr, res->ai_addrlen);
        booted++;
        freeaddrinfo(res);
    }
    dlog("DHT bootstrap: %d routeurs pingues", booted);
    if (booted == 0) {
        if (err) snprintf(err, errlen, "bootstrap DHT injoignable");
        dht_uninit();
        close(s);
        return -1;
    }

    dht_ctx dc = { cb, ctx, 0 };

    u64 freq = armGetSystemTickFreq();
    u64 start = armGetSystemTick();
    u64 last_log = start;
    bool searching = false;
    time_t tosleep = 0;

    while (1) {
        if (cancel && *cancel) break;
        double elapsed_ms = (double)(armGetSystemTick() - start) / freq * 1000.0;
        if (elapsed_ms >= budget_ms) break;
        if (dc.delivered >= target_peers) break;

        // Blocking recvfrom (up to the 1s SO_RCVTIMEO). On a packet, feed it to
        // the DHT; on timeout, tick the DHT with no packet.
        uint8_t buf[3072];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(s, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n > 0) {
            buf[n] = '\0';  // jech expects a NUL-terminated buffer
            dht_periodic(buf, n, (struct sockaddr *)&from, fromlen,
                         &tosleep, on_dht_event, &dc);
        } else {
            dht_periodic(NULL, 0, NULL, 0, &tosleep, on_dht_event, &dc);
        }

        // Once the table has a few nodes, start (and keep) searching.
        int good = 0, dubious = 0;
        dht_nodes(AF_INET, &good, &dubious, NULL, NULL);
        if (!searching && (good + dubious) >= 2) {
            dht_search(info_hash, 0, AF_INET, on_dht_event, &dc);  // port 0 = no announce
            searching = true;
            dlog("DHT: recherche lancee (%d noeuds)", good + dubious);
        }

        if ((double)(armGetSystemTick() - last_log) / freq >= 2.0) {
            dlog("DHT: %d noeuds, %d peers", good + dubious, dc.delivered);
            last_log = armGetSystemTick();
        }
    }

    dlog("DHT fin: %d peers", dc.delivered);
    dht_uninit();
    close(s);
    return dc.delivered;
}
