#include "udpdiag.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <switch.h>

// Builds a BEP15 UDP-tracker "connect" request (16 bytes). We aim it at a
// tracker we KNOW answers (our download code gets peers from it), so the test
// isolates the socket *style* from the destination.
static size_t build_connect_req(uint8_t *out) {
    // protocol_id = 0x41727101980, action = 0 (connect), transaction_id random
    static const uint8_t magic[8] = { 0x00, 0x00, 0x04, 0x17, 0x27, 0x10, 0x19, 0x80 };
    memcpy(out, magic, 8);
    out[8] = out[9] = out[10] = out[11] = 0;  // action = 0
    randomGet(out + 12, 4);                    // transaction_id
    return 16;
}

// Runs three variants of "send a UDP tracker connect, wait for the reply" so we
// can see which socket style actually receives inbound UDP on this device:
//   A: unconnected socket, blocking recvfrom with SO_RCVTIMEO
//   B: unconnected socket, select() then recvfrom (like our DHT / µTP loop)
//   C: connected socket, blocking recv (exactly like our working UDP trackers)
void udp_diagnose(void (*logfn)(const char *)) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo("tracker.opentrackr.org", "1337", &hints, &res) != 0 || !res) {
        logfn("UDPDIAG: tracker resolution failed");
        return;
    }

    uint8_t ping[64];
    size_t plen = build_connect_req(ping);
    uint8_t buf[1500];

    // --- A: unconnected, blocking recvfrom ---
    {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        struct timeval tv = { 3, 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        sendto(s, ping, plen, 0, res->ai_addr, res->ai_addrlen);
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        char m[96];
        snprintf(m, sizeof(m), "UDPDIAG A (unconn+recvfrom bloquant): %ld octets", (long)n);
        logfn(m);
        close(s);
    }

    // --- B: unconnected, select() then recvfrom ---
    {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        sendto(s, ping, plen, 0, res->ai_addr, res->ai_addrlen);
        fd_set r; FD_ZERO(&r); FD_SET(s, &r);
        struct timeval tv = { 3, 0 };
        int rc = select(s + 1, &r, NULL, NULL, &tv);
        ssize_t n = -1;
        if (rc > 0 && FD_ISSET(s, &r)) {
            struct sockaddr_in from; socklen_t fl = sizeof(from);
            n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        }
        char m[96];
        snprintf(m, sizeof(m), "UDPDIAG B (unconn+select): select=%d, %ld octets", rc, (long)n);
        logfn(m);
        close(s);
    }

    // --- C: connected, blocking recv ---
    {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        struct timeval tv = { 3, 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        connect(s, res->ai_addr, res->ai_addrlen);
        send(s, ping, plen, 0);
        ssize_t n = recv(s, buf, sizeof(buf), 0);
        char m[96];
        snprintf(m, sizeof(m), "UDPDIAG C (connecte+recv): %ld octets", (long)n);
        logfn(m);
        close(s);
    }

    freeaddrinfo(res);
}
