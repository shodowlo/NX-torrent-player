#include "udp_tracker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

// BEP 15 magic protocol id used to initiate a connection.
#define UDP_PROTOCOL_ID 0x41727101980ULL

enum { ACTION_CONNECT = 0, ACTION_ANNOUNCE = 1, ACTION_ERROR = 3 };

static void set_err(char *err, size_t errlen, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s", msg);
}

// Splits "udp://host:port/path" into host and port. Returns 0 on success.
static int parse_udp_url(const char *url, char *host, size_t hostlen, char *port, size_t portlen) {
    if (strncmp(url, "udp://", 6) != 0) return -1;
    const char *h = url + 6;

    const char *colon = strchr(h, ':');
    if (!colon) return -1;
    // Port ends at the next '/' or end of string.
    const char *slash = strchr(colon, '/');
    const char *pend = slash ? slash : colon + strlen(colon);

    size_t hlen = (size_t)(colon - h);
    size_t plen = (size_t)(pend - (colon + 1));
    if (hlen == 0 || hlen >= hostlen || plen == 0 || plen >= portlen) return -1;

    memcpy(host, h, hlen); host[hlen] = '\0';
    memcpy(port, colon + 1, plen); port[plen] = '\0';
    return 0;
}

// Big-endian writers into a byte buffer.
static void put32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Sends one datagram and waits for a reply, retrying on timeout. UDP is lossy,
// so BEP 15 mandates retransmission; we cap retries to stay responsive.
static int request_reply(int sock, const uint8_t *req, size_t reqlen,
                         uint8_t *resp, size_t respcap, ssize_t *resplen,
                         uint32_t expect_txid, uint8_t expect_action) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (send(sock, req, reqlen, 0) < 0) return -1;

        ssize_t n = recv(sock, resp, respcap, 0);
        if (n < 16) continue;  // too short or timed out; retry

        uint32_t action = get32(resp);
        uint32_t txid = get32(resp + 4);
        if (txid != expect_txid) continue;
        if (action == ACTION_ERROR) return -2;  // tracker rejected us
        if (action != expect_action) continue;

        *resplen = n;
        return 0;
    }
    return -1;
}

int udp_announce(const char *url, const uint8_t info_hash[20],
                 const uint8_t peer_id[20], int64_t left,
                 peer_addr *peers, int max_peers, char *err, size_t errlen) {
    char host[256], port[16];
    if (parse_udp_url(url, host, sizeof(host), port, sizeof(port)) != 0) {
        set_err(err, errlen, "invalid udp URL");
        return -1;
    }

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        set_err(err, errlen, "DNS resolution failed");
        return -1;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        freeaddrinfo(res);
        set_err(err, errlen, "UDP socket failed");
        return -1;
    }
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        close(sock);
        set_err(err, errlen, "UDP connect failed");
        return -1;
    }
    freeaddrinfo(res);

    srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)&sock);

    // --- Step 1: connect, to obtain a short-lived connection_id ---
    uint8_t creq[16];
    uint32_t ctxid = (uint32_t)rand();
    put64(creq, UDP_PROTOCOL_ID);
    put32(creq + 8, ACTION_CONNECT);
    put32(creq + 12, ctxid);

    uint8_t cresp[64];
    ssize_t clen;
    int rc = request_reply(sock, creq, sizeof(creq), cresp, sizeof(cresp), &clen,
                           ctxid, ACTION_CONNECT);
    if (rc != 0) {
        close(sock);
        set_err(err, errlen, rc == -2 ? "tracker: erreur connect" : "no connect reply");
        return -1;
    }
    uint64_t connection_id = ((uint64_t)get32(cresp + 8) << 32) | get32(cresp + 12);

    // --- Step 2: announce ---
    uint8_t areq[98];
    uint32_t atxid = (uint32_t)rand();
    put64(areq + 0, connection_id);
    put32(areq + 8, ACTION_ANNOUNCE);
    put32(areq + 12, atxid);
    memcpy(areq + 16, info_hash, 20);
    memcpy(areq + 36, peer_id, 20);
    put64(areq + 56, 0);                     // downloaded
    put64(areq + 64, (uint64_t)left);        // left
    put64(areq + 72, 0);                     // uploaded
    put32(areq + 80, 2);                     // event = started
    put32(areq + 84, 0);                     // IP (0 = use source)
    put32(areq + 88, (uint32_t)rand());      // key
    put32(areq + 92, (uint32_t)max_peers);   // num_want
    areq[96] = 0x1A; areq[97] = 0xE1;        // port 6881

    // Response: 20-byte header + 6 bytes per peer.
    uint8_t aresp[20 + 6 * 256];
    ssize_t alen;
    rc = request_reply(sock, areq, sizeof(areq), aresp, sizeof(aresp), &alen,
                       atxid, ACTION_ANNOUNCE);
    close(sock);
    if (rc != 0) {
        set_err(err, errlen, rc == -2 ? "tracker: erreur announce" : "no announce reply");
        return -1;
    }

    int count = 0;
    for (ssize_t off = 20; off + 6 <= alen && count < max_peers; off += 6) {
        memcpy(&peers[count].ip, aresp + off, 4);
        peers[count].port = (uint16_t)((aresp[off + 4] << 8) | aresp[off + 5]);
        if (peers[count].port) count++;
    }
    if (count == 0) {
        set_err(err, errlen, "tracker UDP OK mais 0 peer");
        return -1;
    }
    return count;
}
