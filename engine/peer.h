#ifndef PEER_H
#define PEER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>  // ssize_t

#include "torrent.h"

// BitTorrent peer wire protocol message IDs
enum {
    MSG_CHOKE = 0,
    MSG_UNCHOKE = 1,
    MSG_INTERESTED = 2,
    MSG_NOT_INTERESTED = 3,
    MSG_HAVE = 4,
    MSG_BITFIELD = 5,
    MSG_REQUEST = 6,
    MSG_PIECE = 7,
    MSG_CANCEL = 8,
};

// Blocks are the unit of transfer within a piece (16 KiB is the de-facto standard).
#define BLOCK_LEN (16 * 1024)

// Largest peer-wire message payload we accept/emit (one block plus headroom).
#define MAX_MSG_LEN (BLOCK_LEN + 512)

// TCP connect timeout. Each worker is blocked for this long on every peer that
// never answers, and on a big swarm most peers are NAT'd or dead -- so this
// directly bounds how fast the (few) reachable peers get found. A live peer's
// SYN-ACK lands in tens of milliseconds; 1 s is already generous, and 2 s was
// burning ~94% of all worker time on a 256-peer swarm.
#define PEER_CONNECT_SECS 1

typedef struct {
    int sock;           // TCP socket, or -1 when using µTP
    void *utp;          // µTP connection (utp_conn*), or NULL when using TCP
    bool choked;        // peer is choking us -> we cannot request
    uint8_t *bitfield;  // which pieces the peer has; NULL if not received yet
    size_t bitfield_len;
} peer_conn;

// ---------------------------------------------------------------------------
// Non-blocking peer transport (TCP only), for the poll()-driven session loop.
//
// The blocking peer_conn path above dedicates a thread (and, while parked in
// recv(), one of libnx's 16 BSD sessions) to a single peer, which caps us at
// ~11 concurrent peers. These primitives never block, so one thread can drive
// a hundred peers through a single poll() -- one BSD session for the whole set.
// ---------------------------------------------------------------------------

// Framed reads need to survive short reads, so bytes accumulate here until a
// whole message is present. 4-byte length prefix + the largest message we take.
#define PEER_RX_CAP (4 + MAX_MSG_LEN)

typedef struct {
    int sock;

    uint8_t *rx;        // accumulated inbound bytes (partial messages included)
    size_t rx_off;      // bytes at the front already handed out to the caller
    size_t rx_len;      // total bytes held, rx_off included

    uint8_t *tx;        // queued outbound bytes, drained as the socket allows
    size_t tx_head;     // bytes already sent from the front of tx
    size_t tx_len;
    size_t tx_cap;

    bool handshaked;    // peer's 68-byte handshake received and validated
    bool choked;        // peer is choking us -> we cannot request
    bool we_unchoked;   // we have unchoked the peer

    uint8_t *bitfield;  // which pieces the peer has
    size_t bitfield_len;
} peer_nb;

// Set `sock` non-blocking and attach buffers. Takes ownership of sock.
// Returns 0, or -1 on allocation failure (sock untouched).
int peer_nb_init(peer_nb *p, int sock, int64_t piece_count);
void peer_nb_free(peer_nb *p);

// Drain the socket into the rx buffer. Returns the number of bytes read (0 if
// the socket had nothing), or -1 if the peer closed or errored. The count is
// what tells a genuinely silent peer from a spurious poll wakeup.
ssize_t peer_nb_pump_rx(peer_nb *p);

// Pop one complete message out of the rx buffer, skipping keep-alives.
// Returns 1 and sets *id/*payload/*plen, 0 if no whole message is buffered yet,
// -1 if the peer framed something illegal. *payload points into p->rx and stays
// valid until the next peer_nb_next/peer_nb_pump_rx call.
int peer_nb_next(peer_nb *p, uint8_t *id, uint8_t **payload, uint32_t *plen);

// Queue a framed message. Never blocks; grows the tx buffer as needed.
int peer_nb_queue(peer_nb *p, uint8_t id, const void *payload, uint32_t plen);

// Push as much of the tx queue as the socket accepts. 0 = ok, -1 = dead.
int peer_nb_flush(peer_nb *p);

// True while there are bytes still waiting to go out (poll for POLLOUT).
bool peer_nb_tx_pending(const peer_nb *p);

// Queue our 68-byte handshake. Call once, right after connect() completes.
int peer_nb_send_handshake(peer_nb *p, const uint8_t info_hash[20],
                           const uint8_t peer_id[20]);

// Consume the peer's handshake from rx once it has fully arrived.
// 1 = handshaked ok, 0 = still waiting, -1 = wrong protocol/info_hash.
int peer_nb_recv_handshake(peer_nb *p, const uint8_t info_hash[20]);

// Connect over TCP + handshake + read bitfield. Returns 0 on success.
int peer_connect(peer_conn *p, peer_addr addr, const uint8_t info_hash[20],
                 const uint8_t peer_id[20], int64_t piece_count,
                 char *err, size_t errlen);

// Same, but connects over µTP (requires utp_bridge_init()). Returns 0 on success.
int peer_connect_utp(peer_conn *p, peer_addr addr, const uint8_t info_hash[20],
                     const uint8_t peer_id[20], int64_t piece_count,
                     char *err, size_t errlen);

// Take over an already-accepted incoming TCP socket `fd`: sets options, runs the
// handshake as the receiving side, reads the bitfield. Returns 0 on success.
int peer_accept_incoming(peer_conn *p, int fd, const uint8_t info_hash[20],
                         const uint8_t peer_id[20], int64_t piece_count,
                         char *err, size_t errlen);

void peer_disconnect(peer_conn *p);

bool peer_has_piece(const peer_conn *p, int64_t index);

// Same test against a raw bitfield, so the blocking (peer_conn) and
// non-blocking (peer_nb) paths can share the piece-selection code.
bool bf_has_piece(const uint8_t *bf, size_t bf_len, int64_t index);

// Send interested and wait for unchoke. Returns 0 once unchoked.
int peer_wait_unchoke(peer_conn *p, char *err, size_t errlen);

// Download one full piece into buf (must be >= piece_len bytes).
// Returns 0 on success; verifies nothing (caller checks SHA-1).
int peer_fetch_piece(peer_conn *p, int64_t index, int64_t piece_len,
                     uint8_t *buf, char *err, size_t errlen);

// Low-level message primitives, for callers that run their own session loop
// (e.g. a bidirectional download+upload loop). peer_send writes one message;
// peer_recv reads one (skipping keep-alives), returning the message id and
// setting *plen, or -1 on error/timeout. `payload`/`bufcap` size the buffer.
int peer_send(peer_conn *p, uint8_t id, const void *payload, uint32_t plen);
int peer_recv(peer_conn *p, uint8_t *payload, uint32_t bufcap, uint32_t *plen);

// Connect to a peer and download the torrent metadata (the info dict) via the
// extension protocol (BEP 10) + ut_metadata (BEP 9), given only the info hash.
// On success returns 0 and sets *out to a malloc'd buffer of *out_len bytes
// whose SHA-1 equals info_hash (caller frees). This is how magnet links obtain
// what a .torrent file would have contained.
int peer_fetch_metadata(peer_addr addr, const uint8_t info_hash[20],
                        const uint8_t peer_id[20], uint8_t **out, size_t *out_len,
                        char *err, size_t errlen);

#endif
