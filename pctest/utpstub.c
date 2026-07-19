// µTP is disabled in the PC engine test (TCP-only), so peer.c's µTP path just
// fails gracefully. The real bridge lives in engine/utpbridge.cpp for the Switch.
#include <stddef.h>

#include "utpbridge.h"

int  utp_bridge_init(void) { return 0; }
void utp_bridge_exit(void) {}
utp_conn *utp_bridge_connect(uint32_t ip_net, uint16_t port_host, int timeout_ms) {
    (void)ip_net; (void)port_host; (void)timeout_ms;
    return NULL;   // no µTP on PC test
}
int  utp_bridge_read(utp_conn *c, void *buf, int len, int timeout_ms) {
    (void)c; (void)buf; (void)len; (void)timeout_ms; return -1;
}
int  utp_bridge_write(utp_conn *c, const void *buf, int len) {
    (void)c; (void)buf; (void)len; return -1;
}
void utp_bridge_close(utp_conn *c) { (void)c; }
