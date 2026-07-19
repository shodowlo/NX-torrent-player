#ifndef UTPBRIDGE_H
#define UTPBRIDGE_H

#include <stdint.h>

// Blocking adapter over libutp (µTP). libutp is an async, callback-driven,
// single-threaded transport over one shared UDP socket; our peer-wire code is
// written for blocking sockets. This bridge runs libutp on a background thread
// and exposes each µTP connection as blocking connect/read/write/close, backed
// by ring buffers, so peer.c can use µTP almost exactly like a TCP socket.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct utp_conn utp_conn;

// Start the shared UDP socket and the libutp event loop. Returns 0 on success.
int  utp_bridge_init(void);
void utp_bridge_exit(void);

// Connect to a peer (ip in network byte order, port in host order). Blocks up
// to timeout_ms for the µTP handshake. Returns NULL on failure/timeout.
utp_conn *utp_bridge_connect(uint32_t ip_net, uint16_t port_host, int timeout_ms);

// Blocking read: waits up to timeout_ms for at least one byte. Returns bytes
// read (>0), 0 on clean EOF, or -1 on error/timeout.
int  utp_bridge_read(utp_conn *c, void *buf, int len, int timeout_ms);

// Blocking write of the whole buffer. Returns 0 on success, -1 on error.
int  utp_bridge_write(utp_conn *c, const void *buf, int len);

void utp_bridge_close(utp_conn *c);

#ifdef __cplusplus
}
#endif

#endif
