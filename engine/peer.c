#include "peer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <mbedtls/sha1.h>

#include "bencode.h"
#include "utpbridge.h"

#define HANDSHAKE_LEN 68
#define MSG_EXTENDED 20
#define META_PIECE_LEN 16384
#define MAX_META_SIZE (8 * 1024 * 1024)

static void set_err(char *err, size_t errlen, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s", msg);
}

static int set_timeout(int sock, int seconds) {
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) return -1;
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) return -1;
    return 0;
}

// Blocking connect() waits for the full TCP timeout (~30s) on an unreachable
// peer, which stalls the swarm. A non-blocking connect + select lets us give
// up after a few seconds and move on to another peer.
static int connect_timeout(int sock, const struct sockaddr *sa, socklen_t salen,
                           int seconds) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return -1;
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(sock, sa, salen);
    if (rc == 0) {
        fcntl(sock, F_SETFL, flags);
        return 0;  // connected immediately
    }
    if (errno != EINPROGRESS) {
        fcntl(sock, F_SETFL, flags);
        return -1;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
    rc = select(sock + 1, NULL, &wfds, NULL, &tv);
    if (rc <= 0) {
        fcntl(sock, F_SETFL, flags);
        return -1;  // timeout or error
    }

    int soerr = 0;
    socklen_t len = sizeof(soerr);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0 || soerr != 0) {
        fcntl(sock, F_SETFL, flags);
        return -1;
    }

    fcntl(sock, F_SETFL, flags);  // back to blocking for the transfer phase
    return 0;
}

// Sockets can return short reads/writes; loop until the full buffer moves.
// Transport read/write dispatch: a peer_conn is either a TCP socket or a µTP
// connection. Everything else in this file goes through these two helpers, so
// the peer-wire protocol is identical over both transports.
#define UTP_IO_TIMEOUT_MS 10000

// Returns 0 on success, -1 on error/EOF, and -2 on a clean idle timeout — the
// socket's SO_RCVTIMEO fired at a message boundary with nothing read yet. -2
// lets the caller poll for shutdown without dropping a slow-but-alive peer.
static int read_full(peer_conn *p, void *buf, size_t len) {
    uint8_t *b = buf;
    size_t got = 0;
    while (got < len) {
        if (p->utp) {
            int n = utp_bridge_read(p->utp, b + got, (int)(len - got),
                                    UTP_IO_TIMEOUT_MS);
            if (n <= 0) return -1;
            got += (size_t)n;
        } else {
            ssize_t n = recv(p->sock, b + got, len - got, 0);
            if (n == 0) return -1;  // peer closed
            if (n < 0) {
                if ((errno == EAGAIN || errno == EWOULDBLOCK) && got == 0)
                    return -2;      // idle timeout at a message boundary
                return -1;          // real error, or timeout mid-message
            }
            got += (size_t)n;
        }
    }
    return 0;
}

static int write_full(peer_conn *p, const void *buf, size_t len) {
    if (p->utp)
        return utp_bridge_write(p->utp, buf, (int)len);  // writes the whole buffer

    const uint8_t *b = buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(p->sock, b + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

// Message framing: <4-byte big-endian length><1-byte id><payload>
// A zero length is a keep-alive and carries no id.
static int send_msg(peer_conn *p, uint8_t id, const void *payload, uint32_t plen) {
    uint8_t hdr[5];
    uint32_t len = htonl(1 + plen);
    memcpy(hdr, &len, 4);
    hdr[4] = id;
    if (write_full(p, hdr, 5) != 0) return -1;
    if (plen && write_full(p, payload, plen) != 0) return -1;
    return 0;
}

// Reads one message, skipping keep-alives. Caller supplies the payload buffer.
// Returns the message id, or -1 on error. *plen receives the payload length.
static int recv_msg(peer_conn *p, uint8_t *payload, uint32_t bufcap, uint32_t *plen) {
    for (;;) {
        uint32_t len_be;
        int r = read_full(p, &len_be, 4);
        if (r == -2) return -2;  // idle timeout: no message pending
        if (r != 0) return -1;
        uint32_t len = ntohl(len_be);
        if (len == 0) continue;  // keep-alive
        if (len > MAX_MSG_LEN) return -1;

        uint8_t id;
        if (read_full(p, &id, 1) != 0) return -1;
        uint32_t payload_len = len - 1;
        if (payload_len > bufcap) return -1;
        if (payload_len && read_full(p, payload, payload_len) != 0) return -1;
        *plen = payload_len;
        return id;
    }
}

int peer_send(peer_conn *p, uint8_t id, const void *payload, uint32_t plen) {
    return send_msg(p, id, payload, plen);
}

int peer_recv(peer_conn *p, uint8_t *payload, uint32_t bufcap, uint32_t *plen) {
    return recv_msg(p, payload, bufcap, plen);
}

// Handshake + initial bitfield/have exchange over whatever transport `p` is
// already set up on (TCP or µTP). p->sock / p->utp must be ready. Returns 0 on
// success (allocating p->bitfield); on failure returns -1 and the caller tears
// down the transport with peer_disconnect.
static int peer_handshake(peer_conn *p, const uint8_t info_hash[20],
                          const uint8_t peer_id[20], int64_t piece_count,
                          char *err, size_t errlen) {
    // Handshake: <19><"BitTorrent protocol"><8 reserved><info_hash><peer_id>
    uint8_t hs[HANDSHAKE_LEN];
    hs[0] = 19;
    memcpy(hs + 1, "BitTorrent protocol", 19);
    memset(hs + 20, 0, 8);
    memcpy(hs + 28, info_hash, 20);
    memcpy(hs + 48, peer_id, 20);

    if (write_full(p, hs, HANDSHAKE_LEN) != 0) {
        set_err(err, errlen, "handshake send failed");
        return -1;
    }

    uint8_t resp[HANDSHAKE_LEN];
    if (read_full(p, resp, HANDSHAKE_LEN) != 0) {
        set_err(err, errlen, "no handshake reply");
        return -1;
    }
    if (resp[0] != 19 || memcmp(resp + 1, "BitTorrent protocol", 19) != 0) {
        set_err(err, errlen, "unknown protocol");
        return -1;
    }
    if (memcmp(resp + 28, info_hash, 20) != 0) {  // same torrent?
        set_err(err, errlen, "info_hash mismatch");
        return -1;
    }

    p->bitfield_len = (size_t)((piece_count + 7) / 8);
    p->bitfield = calloc(1, p->bitfield_len);
    if (!p->bitfield) { set_err(err, errlen, "out of memory"); return -1; }

    // A bitfield, if sent at all, must be the first message. Peers may instead
    // send nothing (they have no pieces) or a stream of have messages.
    uint8_t *payload = malloc(MAX_MSG_LEN);
    if (!payload) { set_err(err, errlen, "out of memory"); return -1; }

    for (int i = 0; i < 8; i++) {
        uint32_t plen = 0;
        int id = recv_msg(p, payload, MAX_MSG_LEN, &plen);
        if (id < 0) break;  // no bitfield is not fatal

        if (id == MSG_BITFIELD && plen == p->bitfield_len) {
            memcpy(p->bitfield, payload, plen);
            break;
        }
        if (id == MSG_HAVE && plen == 4) {
            uint32_t idx;
            memcpy(&idx, payload, 4);
            idx = ntohl(idx);
            if (idx < (uint32_t)piece_count)
                p->bitfield[idx / 8] |= (uint8_t)(0x80 >> (idx % 8));
            continue;
        }
        if (id == MSG_UNCHOKE) p->choked = false;
        if (id == MSG_CHOKE) p->choked = true;
    }
    free(payload);
    return 0;
}

int peer_connect(peer_conn *p, peer_addr addr, const uint8_t info_hash[20],
                 const uint8_t peer_id[20], int64_t piece_count,
                 char *err, size_t errlen) {
    memset(p, 0, sizeof(*p));
    p->sock = -1;
    p->choked = true;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { set_err(err, errlen, "socket() failed"); return -1; }
    // Short recv timeout: the session loop treats a timeout as "poll for stop and
    // keep waiting" (see run_session), so this bounds how long a worker blocked on
    // a quiet peer takes to notice cancellation — not how long we tolerate a slow
    // peer. 1 s keeps teardown (and switching streams) snappy.
    set_timeout(sock, 1);

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(addr.port);
    sa.sin_addr.s_addr = addr.ip;

    if (connect_timeout(sock, (struct sockaddr *)&sa, sizeof(sa), PEER_CONNECT_SECS) != 0) {
        close(sock);
        set_err(err, errlen, "connection refused or timed out");
        return -1;
    }

    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    p->sock = sock;

    if (peer_handshake(p, info_hash, peer_id, piece_count, err, errlen) != 0) {
        peer_disconnect(p);
        return -1;
    }
    return 0;
}

int peer_accept_incoming(peer_conn *p, int fd, const uint8_t info_hash[20],
                         const uint8_t peer_id[20], int64_t piece_count,
                         char *err, size_t errlen) {
    memset(p, 0, sizeof(*p));
    p->sock = fd;
    p->choked = true;
    set_timeout(fd, 1);  // short recv timeout for responsive cancel (see run_session)
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // The BitTorrent handshake is symmetric (both sides send the same 68-byte
    // frame). peer_handshake writes ours then reads/validates theirs, which works
    // for an accepted connection too since we serve a single, known info_hash.
    if (peer_handshake(p, info_hash, peer_id, piece_count, err, errlen) != 0) {
        peer_disconnect(p);
        return -1;
    }
    return 0;
}

int peer_connect_utp(peer_conn *p, peer_addr addr, const uint8_t info_hash[20],
                     const uint8_t peer_id[20], int64_t piece_count,
                     char *err, size_t errlen) {
    memset(p, 0, sizeof(*p));
    p->sock = -1;
    p->choked = true;

    p->utp = utp_bridge_connect(addr.ip, addr.port, 4000);
    if (!p->utp) { set_err(err, errlen, "uTP connection failed"); return -1; }

    if (peer_handshake(p, info_hash, peer_id, piece_count, err, errlen) != 0) {
        peer_disconnect(p);
        return -1;
    }
    return 0;
}

void peer_disconnect(peer_conn *p) {
    if (p->utp) { utp_bridge_close(p->utp); p->utp = NULL; }
    if (p->sock >= 0) close(p->sock);
    free(p->bitfield);
    p->sock = -1;
    p->bitfield = NULL;
    p->bitfield_len = 0;
}

bool bf_has_piece(const uint8_t *bf, size_t bf_len, int64_t index) {
    if (!bf || index < 0) return false;
    size_t byte = (size_t)(index / 8);
    if (byte >= bf_len) return false;
    // Bits are packed high-to-low within each byte.
    return (bf[byte] & (0x80 >> (index % 8))) != 0;
}

bool peer_has_piece(const peer_conn *p, int64_t index) {
    return bf_has_piece(p->bitfield, p->bitfield_len, index);
}

int peer_wait_unchoke(peer_conn *p, char *err, size_t errlen) {
    if (send_msg(p, MSG_INTERESTED, NULL, 0) != 0) {
        set_err(err, errlen, "interested send failed");
        return -1;
    }
    if (!p->choked) return 0;

    uint8_t *payload = malloc(MAX_MSG_LEN);
    if (!payload) { set_err(err, errlen, "out of memory"); return -1; }

    // A busy peer may send several messages before it gets around to unchoking.
    for (int i = 0; i < 32; i++) {
        uint32_t plen = 0;
        int id = recv_msg(p, payload, MAX_MSG_LEN, &plen);
        if (id < 0) {
            free(payload);
            set_err(err, errlen, "deconnecte en attendant unchoke");
            return -1;
        }
        if (id == MSG_UNCHOKE) {
            p->choked = false;
            free(payload);
            return 0;
        }
        if (id == MSG_CHOKE) p->choked = true;
    }
    free(payload);
    set_err(err, errlen, "peer refuse de nous unchoke");
    return -1;
}

// Number of block requests kept in flight at once. Stop-and-wait (1) caps
// throughput at block_size/RTT; pipelining hides latency and is the single
// biggest speed win for the peer protocol. 32 blocks * 16 KB = 512 KB in
// flight, enough to keep a full typical piece requested at once.
#define PIPELINE_DEPTH 32

static int send_request(peer_conn *p, uint32_t index, uint32_t begin, uint32_t len) {
    uint8_t req[12];
    uint32_t v;
    v = htonl(index); memcpy(req + 0, &v, 4);
    v = htonl(begin); memcpy(req + 4, &v, 4);
    v = htonl(len);   memcpy(req + 8, &v, 4);
    return send_msg(p, MSG_REQUEST, req, 12);
}

static uint32_t block_length(int64_t piece_len, int block_idx) {
    int64_t begin = (int64_t)block_idx * BLOCK_LEN;
    int64_t rem = piece_len - begin;
    return rem > BLOCK_LEN ? BLOCK_LEN : (uint32_t)rem;
}

int peer_fetch_piece(peer_conn *p, int64_t index, int64_t piece_len,
                     uint8_t *buf, char *err, size_t errlen) {
    if (p->choked) { set_err(err, errlen, "peer nous a choke"); return -1; }

    uint8_t *payload = malloc(MAX_MSG_LEN);
    if (!payload) { set_err(err, errlen, "out of memory"); return -1; }

    int nblocks = (int)((piece_len + BLOCK_LEN - 1) / BLOCK_LEN);
    bool got[nblocks];
    memset(got, 0, sizeof(got));

    int requested = 0;   // blocks for which a request has been sent
    int received = 0;    // distinct blocks fully received

    // Prime the pipeline with the first window of requests.
    while (requested < nblocks && requested < PIPELINE_DEPTH) {
        if (send_request(p, (uint32_t)index,
                         (uint32_t)requested * BLOCK_LEN,
                         block_length(piece_len, requested)) != 0) {
            free(payload);
            set_err(err, errlen, "request send failed");
            return -1;
        }
        requested++;
    }

    // Bound the number of messages we'll read before giving up on a stalled
    // peer, generous enough to absorb keep-alives and have messages.
    int budget = nblocks * 4 + 64;

    while (received < nblocks && budget-- > 0) {
        uint32_t plen = 0;
        int id = recv_msg(p, payload, MAX_MSG_LEN, &plen);
        if (id < 0) {
            free(payload);
            set_err(err, errlen, "deconnecte pendant le transfert");
            return -1;
        }
        if (id == MSG_CHOKE) {
            p->choked = true;
            free(payload);
            set_err(err, errlen, "choke pendant le transfert");
            return -1;
        }
        if (id != MSG_PIECE || plen < 8) continue;

        // piece: <index><begin><block>
        uint32_t r_index, r_begin;
        memcpy(&r_index, payload + 0, 4); r_index = ntohl(r_index);
        memcpy(&r_begin, payload + 4, 4); r_begin = ntohl(r_begin);
        uint32_t blen = plen - 8;

        if (r_index != (uint32_t)index) continue;
        if (r_begin % BLOCK_LEN != 0) continue;
        int bidx = (int)(r_begin / BLOCK_LEN);
        if (bidx < 0 || bidx >= nblocks) continue;
        if (blen != block_length(piece_len, bidx)) continue;
        if (got[bidx]) continue;  // duplicate

        memcpy(buf + r_begin, payload + 8, blen);
        got[bidx] = true;
        received++;

        // Slide the window: for each block received, request one more.
        if (requested < nblocks) {
            if (send_request(p, (uint32_t)index,
                             (uint32_t)requested * BLOCK_LEN,
                             block_length(piece_len, requested)) != 0) {
                free(payload);
                set_err(err, errlen, "request send failed");
                return -1;
            }
            requested++;
        }
    }

    free(payload);
    if (received < nblocks) {
        set_err(err, errlen, "incomplete piece (peer too slow)");
        return -1;
    }
    return 0;
}

//---------------------------------------------------------------------------
// Metadata download (BEP 10 extension protocol + BEP 9 ut_metadata)
//---------------------------------------------------------------------------

// Sends an extended message: <len><20><ext_sub_id><payload>.
static int send_ext(peer_conn *p, uint8_t sub_id, const char *bencode, size_t blen) {
    uint8_t msg[128];
    if (blen + 1 > sizeof(msg)) return -1;
    msg[0] = sub_id;
    memcpy(msg + 1, bencode, blen);
    return send_msg(p, MSG_EXTENDED, msg, (uint32_t)(blen + 1));
}

int peer_fetch_metadata(peer_addr addr, const uint8_t info_hash[20],
                        const uint8_t peer_id[20], uint8_t **out, size_t *out_len,
                        char *err, size_t errlen) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { set_err(err, errlen, "socket"); return -1; }
    set_timeout(sock, 10);

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(addr.port);
    sa.sin_addr.s_addr = addr.ip;
    if (connect_timeout(sock, (struct sockaddr *)&sa, sizeof(sa), PEER_CONNECT_SECS) != 0) {
        close(sock);
        set_err(err, errlen, "connection refused");
        return -1;
    }

    // Metadata fetch is TCP-only; wrap the socket so it can use the shared
    // transport helpers (send_ext/recv_msg/read_full/write_full).
    peer_conn mp = {0};
    mp.sock = sock;
    peer_conn *p = &mp;

    // Handshake with the extension-protocol bit (reserved byte 5, 0x10) set.
    uint8_t hs[HANDSHAKE_LEN];
    hs[0] = 19;
    memcpy(hs + 1, "BitTorrent protocol", 19);
    memset(hs + 20, 0, 8);
    hs[25] = 0x10;
    memcpy(hs + 28, info_hash, 20);
    memcpy(hs + 48, peer_id, 20);
    if (write_full(p, hs, HANDSHAKE_LEN) != 0) {
        close(sock); set_err(err, errlen, "handshake send failed"); return -1;
    }
    uint8_t resp[HANDSHAKE_LEN];
    if (read_full(p, resp, HANDSHAKE_LEN) != 0) {
        close(sock); set_err(err, errlen, "no handshake"); return -1;
    }
    if (memcmp(resp + 28, info_hash, 20) != 0) {
        close(sock); set_err(err, errlen, "info_hash mismatch"); return -1;
    }
    if (!(resp[25] & 0x10)) {
        close(sock); set_err(err, errlen, "peer without extensions"); return -1;
    }

    // Advertise ut_metadata=1 to the peer.
    static const char my_ehs[] = "d1:md11:ut_metadatai1eee";
    if (send_ext(p, 0, my_ehs, sizeof(my_ehs) - 1) != 0) {
        close(sock); set_err(err, errlen, "ext handshake send failed"); return -1;
    }

    uint8_t *payload = malloc(MAX_MSG_LEN);
    if (!payload) { close(sock); set_err(err, errlen, "out of memory"); return -1; }

    // Wait for the peer's extended handshake to learn its ut_metadata id + size.
    int peer_ut_id = -1;
    int64_t meta_size = -1;
    for (int i = 0; i < 16; i++) {
        uint32_t plen = 0;
        int id = recv_msg(p, payload, MAX_MSG_LEN, &plen);
        if (id < 0) break;
        if (id != MSG_EXTENDED || plen < 1 || payload[0] != 0) continue;

        be_node *d = be_parse((const char *)payload + 1, plen - 1);
        if (d) {
            be_node *msize = be_dict_get(d, "metadata_size");
            be_node *m = be_dict_get(d, "m");
            be_node *utm = m ? be_dict_get(m, "ut_metadata") : NULL;
            if (utm && utm->type == BE_INT) peer_ut_id = (int)utm->i;
            if (msize && msize->type == BE_INT) meta_size = msize->i;
            be_free(d);
        }
        break;
    }
    if (peer_ut_id <= 0 || meta_size <= 0 || meta_size > MAX_META_SIZE) {
        free(payload); close(sock);
        set_err(err, errlen, "peer does not share metadata");
        return -1;
    }

    uint8_t *meta = malloc(meta_size);
    int npieces = (int)((meta_size + META_PIECE_LEN - 1) / META_PIECE_LEN);
    bool *got = calloc(npieces, 1);
    if (!meta || !got) {
        free(meta); free(got); free(payload); close(sock);
        set_err(err, errlen, "out of memory"); return -1;
    }

    // Request every metadata piece up front.
    for (int i = 0; i < npieces; i++) {
        char req[64];
        int n = snprintf(req, sizeof(req), "d8:msg_typei0e5:piecei%dee", i);
        send_ext(p, (uint8_t)peer_ut_id, req, (size_t)n);
    }

    // Collect data messages (ext sub-id == our advertised id, 1).
    int received = 0;
    int budget = npieces * 4 + 64;
    while (received < npieces && budget-- > 0) {
        uint32_t plen = 0;
        int id = recv_msg(p, payload, MAX_MSG_LEN, &plen);
        if (id < 0) break;
        if (id != MSG_EXTENDED || plen < 1 || payload[0] != 1) continue;

        be_node *d = be_parse((const char *)payload + 1, plen - 1);
        if (!d) continue;
        be_node *mt = be_dict_get(d, "msg_type");
        be_node *pc = be_dict_get(d, "piece");
        bool is_data = mt && mt->type == BE_INT && mt->i == 1 &&
                       pc && pc->type == BE_INT;
        int idx = is_data ? (int)pc->i : -1;
        size_t header_len = 1 + d->rawlen;  // sub-id byte + bencode dict
        be_free(d);

        if (!is_data || idx < 0 || idx >= npieces || got[idx]) continue;

        int64_t expect = (idx == npieces - 1)
                             ? meta_size - (int64_t)idx * META_PIECE_LEN
                             : META_PIECE_LEN;
        if ((int64_t)plen - (int64_t)header_len < expect) continue;

        memcpy(meta + (int64_t)idx * META_PIECE_LEN, payload + header_len, expect);
        got[idx] = true;
        received++;
    }

    free(payload);
    free(got);
    close(sock);

    if (received < npieces) {
        free(meta);
        set_err(err, errlen, "incomplete metadata");
        return -1;
    }

    // The reassembled metadata must hash to the info hash we asked for.
    uint8_t hash[20];
    mbedtls_sha1(meta, meta_size, hash);
    if (memcmp(hash, info_hash, 20) != 0) {
        free(meta);
        set_err(err, errlen, "invalid metadata hash");
        return -1;
    }

    *out = meta;
    *out_len = meta_size;
    return 0;
}

// ---------------------------------------------------------------------------
// Non-blocking peer transport (TCP only). See the block comment in peer.h.
// ---------------------------------------------------------------------------

// Slide any fully-consumed prefix out of the rx buffer. Doing this lazily (only
// when we need room or run out of whole messages) is what lets peer_nb_next
// hand back a pointer straight into rx instead of copying every payload.
static void nb_compact(peer_nb *p) {
    if (p->rx_off == 0) return;
    if (p->rx_len > p->rx_off)
        memmove(p->rx, p->rx + p->rx_off, p->rx_len - p->rx_off);
    p->rx_len -= p->rx_off;
    p->rx_off = 0;
}

int peer_nb_init(peer_nb *p, int sock, int64_t piece_count) {
    memset(p, 0, sizeof(*p));
    p->sock         = sock;
    p->choked       = true;
    p->bitfield_len = (size_t)((piece_count + 7) / 8);
    p->tx_cap       = 4096;
    p->rx           = malloc(PEER_RX_CAP);
    p->tx           = malloc(p->tx_cap);
    p->bitfield     = calloc(1, p->bitfield_len ? p->bitfield_len : 1);
    if (!p->rx || !p->tx || !p->bitfield) {
        free(p->rx);
        free(p->tx);
        free(p->bitfield);
        memset(p, 0, sizeof(*p));
        p->sock = -1;
        return -1;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return 0;
}

void peer_nb_free(peer_nb *p) {
    if (p->sock >= 0) close(p->sock);
    free(p->rx);
    free(p->tx);
    free(p->bitfield);
    memset(p, 0, sizeof(*p));
    p->sock = -1;
}

ssize_t peer_nb_pump_rx(peer_nb *p) {
    ssize_t total = 0;
    for (;;) {
        if (p->rx_len >= PEER_RX_CAP) {
            nb_compact(p);
            if (p->rx_len >= PEER_RX_CAP)
                return total;  // a whole message is buffered; caller must drain it
        }
        ssize_t n = recv(p->sock, p->rx + p->rx_len, PEER_RX_CAP - p->rx_len, 0);
        if (n > 0) {
            p->rx_len += (size_t)n;
            total += n;
            continue;
        }
        if (n == 0) return -1;  // peer closed
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return total;
        return -1;
    }
}

int peer_nb_next(peer_nb *p, uint8_t *id, uint8_t **payload, uint32_t *plen) {
    for (;;) {
        size_t avail = p->rx_len - p->rx_off;
        if (avail < 4) {
            nb_compact(p);
            return 0;
        }
        uint32_t len_be;
        memcpy(&len_be, p->rx + p->rx_off, 4);
        uint32_t len = ntohl(len_be);
        if (len == 0) {  // keep-alive carries no id
            p->rx_off += 4;
            continue;
        }
        if (len > MAX_MSG_LEN) return -1;
        if (avail < 4 + (size_t)len) {
            nb_compact(p);
            return 0;  // message still on the wire
        }
        *id      = p->rx[p->rx_off + 4];
        *plen    = len - 1;
        *payload = p->rx + p->rx_off + 5;
        p->rx_off += 4 + (size_t)len;
        return 1;
    }
}

// Append raw bytes to the tx queue, growing it if needed.
static int nb_tx_append(peer_nb *p, const void *buf, size_t len) {
    if (p->tx_head > 0 && p->tx_head == p->tx_len) {
        p->tx_head = p->tx_len = 0;  // fully drained: reuse from the front
    }
    if (p->tx_len + len > p->tx_cap) {
        if (p->tx_head > 0) {  // reclaim the drained prefix before growing
            memmove(p->tx, p->tx + p->tx_head, p->tx_len - p->tx_head);
            p->tx_len -= p->tx_head;
            p->tx_head = 0;
        }
        while (p->tx_len + len > p->tx_cap) {
            size_t cap = p->tx_cap * 2;
            // Serving blocks queues 16 KB a piece; cap the backlog so a peer
            // that never drains cannot balloon us out of memory.
            if (cap > 512 * 1024) return -1;
            uint8_t *nt = realloc(p->tx, cap);
            if (!nt) return -1;
            p->tx     = nt;
            p->tx_cap = cap;
        }
    }
    memcpy(p->tx + p->tx_len, buf, len);
    p->tx_len += len;
    return 0;
}

int peer_nb_queue(peer_nb *p, uint8_t id, const void *payload, uint32_t plen) {
    uint8_t hdr[5];
    uint32_t len = htonl(1 + plen);
    memcpy(hdr, &len, 4);
    hdr[4] = id;
    if (nb_tx_append(p, hdr, 5) != 0) return -1;
    if (plen && nb_tx_append(p, payload, plen) != 0) return -1;
    return 0;
}

int peer_nb_flush(peer_nb *p) {
    while (p->tx_head < p->tx_len) {
        ssize_t n = send(p->sock, p->tx + p->tx_head, p->tx_len - p->tx_head, 0);
        if (n > 0) {
            p->tx_head += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1;
    }
    p->tx_head = p->tx_len = 0;
    return 0;
}

bool peer_nb_tx_pending(const peer_nb *p) {
    return p->tx_head < p->tx_len;
}

int peer_nb_send_handshake(peer_nb *p, const uint8_t info_hash[20],
                           const uint8_t peer_id[20]) {
    uint8_t hs[HANDSHAKE_LEN];
    hs[0] = 19;
    memcpy(hs + 1, "BitTorrent protocol", 19);
    memset(hs + 20, 0, 8);  // no extensions advertised
    memcpy(hs + 28, info_hash, 20);
    memcpy(hs + 48, peer_id, 20);
    return nb_tx_append(p, hs, HANDSHAKE_LEN);
}

int peer_nb_recv_handshake(peer_nb *p, const uint8_t info_hash[20]) {
    if (p->rx_len - p->rx_off < HANDSHAKE_LEN) {
        nb_compact(p);
        return 0;  // not all 68 bytes yet
    }
    const uint8_t *hs = p->rx + p->rx_off;
    if (hs[0] != 19 || memcmp(hs + 1, "BitTorrent protocol", 19) != 0) return -1;
    if (memcmp(hs + 28, info_hash, 20) != 0) return -1;  // different torrent
    p->rx_off += HANDSHAKE_LEN;
    p->handshaked = true;
    nb_compact(p);
    return 1;
}
