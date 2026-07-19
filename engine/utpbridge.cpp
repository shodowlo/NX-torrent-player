#include "utpbridge.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <switch.h>

#include "utp.h"

// Connection buffer sizes. Receive is generous (a peer can burst several piece
// blocks); send only needs to hold a request/piece in flight.
#define RBUF_SIZE (1024 * 1024)
#define WBUF_SIZE (256 * 1024)

enum { ST_CONNECTING = 0, ST_CONNECTED = 1, ST_EOF = 2, ST_ERROR = 3 };

struct utp_conn {
    UTPSocket *sock;
    int state;
    bool closing;
    int refs;             // app + libutp; freed when it hits 0

    Mutex m;              // guards the buffers + state below
    CondVar rcond;        // signaled when rbuf gains data or state changes
    CondVar wcond;        // signaled when wbuf drains or state changes

    uint8_t *rbuf; size_t rhead, rcount;   // received, not yet read by app
    uint8_t *wbuf; size_t whead, wcount;   // queued by app, not yet sent
};

// One UDP socket and one libutp event loop for the whole app.
static int    g_udp = -1;
static Mutex  g_lock;          // serializes ALL libutp calls (it is not reentrant)
static Thread g_thread;
static bool   g_started;
static volatile bool g_stop;

//-----------------------------------------------------------------------------
// Ring-buffer helpers (caller holds c->m)
//-----------------------------------------------------------------------------

static size_t rb_free(utp_conn *c) { return RBUF_SIZE - c->rcount; }
static size_t wb_free(utp_conn *c) { return WBUF_SIZE - c->wcount; }

static void rb_push(utp_conn *c, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        size_t tail = (c->rhead + c->rcount) % RBUF_SIZE;
        c->rbuf[tail] = p[i];
        c->rcount++;
    }
}
static size_t rb_pop(utp_conn *c, uint8_t *p, size_t n) {
    size_t got = 0;
    while (got < n && c->rcount > 0) {
        p[got++] = c->rbuf[c->rhead];
        c->rhead = (c->rhead + 1) % RBUF_SIZE;
        c->rcount--;
    }
    return got;
}
static void wb_push(utp_conn *c, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        size_t tail = (c->whead + c->wcount) % WBUF_SIZE;
        c->wbuf[tail] = p[i];
        c->wcount++;
    }
}
static size_t wb_pop(utp_conn *c, uint8_t *p, size_t n) {
    size_t got = 0;
    while (got < n && c->wcount > 0) {
        p[got++] = c->wbuf[c->whead];
        c->whead = (c->whead + 1) % WBUF_SIZE;
        c->wcount--;
    }
    return got;
}

//-----------------------------------------------------------------------------
// libutp callbacks (all invoked from inside a libutp call, i.e. under g_lock)
//-----------------------------------------------------------------------------

static void cb_on_read(void *u, const byte *bytes, size_t count) {
    utp_conn *c = (utp_conn *)u;
    mutexLock(&c->m);
    size_t n = count < rb_free(c) ? count : rb_free(c);  // flow-controlled by get_rb_size
    rb_push(c, bytes, n);
    condvarWakeAll(&c->rcond);
    mutexUnlock(&c->m);
}

static void cb_on_write(void *u, byte *bytes, size_t count) {
    utp_conn *c = (utp_conn *)u;
    mutexLock(&c->m);
    wb_pop(c, bytes, count);   // count <= wcount by construction
    condvarWakeAll(&c->wcond);
    mutexUnlock(&c->m);
}

static size_t cb_get_rb_size(void *u) {
    utp_conn *c = (utp_conn *)u;
    mutexLock(&c->m);
    size_t n = c->rcount;
    mutexUnlock(&c->m);
    return n;
}

// Pushes as many buffered send bytes as the window allows. Caller holds g_lock.
static void drain_wbuf(utp_conn *c) {
    mutexLock(&c->m);
    size_t n = c->wcount;
    mutexUnlock(&c->m);
    if (n > 0 && c->sock && !c->closing) UTP_Write(c->sock, n);
}

static void free_conn(utp_conn *c) {
    free(c->rbuf);
    free(c->wbuf);
    free(c);
}

// Caller holds g_lock.
static void decref(utp_conn *c) {
    if (--c->refs == 0) free_conn(c);
}

static void cb_on_state(void *u, int state) {
    utp_conn *c = (utp_conn *)u;
    if (state == UTP_STATE_CONNECT || state == UTP_STATE_WRITABLE) {
        mutexLock(&c->m);
        c->state = ST_CONNECTED;
        condvarWakeAll(&c->rcond);
        condvarWakeAll(&c->wcond);
        mutexUnlock(&c->m);
        drain_wbuf(c);  // window opened: push queued bytes
    } else if (state == UTP_STATE_EOF) {
        mutexLock(&c->m);
        c->state = ST_EOF;
        condvarWakeAll(&c->rcond);
        condvarWakeAll(&c->wcond);
        mutexUnlock(&c->m);
    } else if (state == UTP_STATE_DESTROYING) {
        c->sock = NULL;
        decref(c);   // libutp releases its reference (under g_lock)
    }
}

static void cb_on_error(void *u, int err) {
    (void)err;
    utp_conn *c = (utp_conn *)u;
    mutexLock(&c->m);
    if (c->state != ST_EOF) c->state = ST_ERROR;
    condvarWakeAll(&c->rcond);
    condvarWakeAll(&c->wcond);
    mutexUnlock(&c->m);
}

static void cb_on_overhead(void *u, bool s, size_t c, int t) {
    (void)u; (void)s; (void)c; (void)t;
}

static UTPFunctionTable g_functable = {
    cb_on_read, cb_on_write, cb_get_rb_size, cb_on_state, cb_on_error, cb_on_overhead
};

// libutp asks us to send a UDP datagram.
static void cb_send_to(void *u, const byte *p, size_t len,
                       const struct sockaddr *to, socklen_t tolen) {
    (void)u;
    if (g_udp >= 0) sendto(g_udp, p, len, 0, to, tolen);
}

// We do not accept inbound µTP for now; reject any incoming connection.
static void cb_on_incoming(void *u, UTPSocket *s) {
    (void)u;
    UTP_Close(s);
}

//-----------------------------------------------------------------------------
// Event loop
//-----------------------------------------------------------------------------

static void utp_loop(void *) {
    uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) return;

    // Switch's select() does not report UDP readability, so we use a blocking
    // recvfrom with a 50ms receive timeout (set in utp_bridge_init): on a
    // packet we route it, and either way we tick UTP_CheckTimeouts.
    while (!g_stop) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(g_udp, buf, 4096, 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n > 0) {
            mutexLock(&g_lock);
            UTP_IsIncomingUTP(cb_on_incoming, cb_send_to, NULL,
                              buf, (size_t)n, (struct sockaddr *)&from, fromlen);
            mutexUnlock(&g_lock);
        }

        mutexLock(&g_lock);
        UTP_CheckTimeouts();
        mutexUnlock(&g_lock);
    }
    free(buf);
}

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------

int utp_bridge_init(void) {
    if (g_started) return 0;

    g_udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp < 0) return -1;
    struct sockaddr_in me = {};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = INADDR_ANY;
    me.sin_port = 0;  // ephemeral
    bind(g_udp, (struct sockaddr *)&me, sizeof(me));

    // 50ms recv timeout so the loop ticks UTP_CheckTimeouts regularly without
    // select() (which does not work for UDP on Switch).
    struct timeval rcvto = { 0, 50000 };
    setsockopt(g_udp, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof(rcvto));

    mutexInit(&g_lock);
    g_stop = false;

    if (threadCreate(&g_thread, utp_loop, NULL, NULL, 0x8000, 0x2C, -2) != 0) {
        close(g_udp);
        g_udp = -1;
        return -1;
    }
    threadStart(&g_thread);
    g_started = true;
    return 0;
}

void utp_bridge_exit(void) {
    if (!g_started) return;
    g_stop = true;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    close(g_udp);
    g_udp = -1;
    g_started = false;
}

static u64 ms_to_ns(int ms) { return (u64)ms * 1000000ULL; }

utp_conn *utp_bridge_connect(uint32_t ip_net, uint16_t port_host, int timeout_ms) {
    if (!g_started) return NULL;

    utp_conn *c = (utp_conn *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->rbuf = (uint8_t *)malloc(RBUF_SIZE);
    c->wbuf = (uint8_t *)malloc(WBUF_SIZE);
    if (!c->rbuf || !c->wbuf) { free_conn(c); return NULL; }
    c->state = ST_CONNECTING;
    c->refs = 2;  // app + libutp
    mutexInit(&c->m);
    condvarInit(&c->rcond);
    condvarInit(&c->wcond);

    struct sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = ip_net;
    sa.sin_port = htons(port_host);

    mutexLock(&g_lock);
    c->sock = UTP_Create(cb_send_to, NULL, (struct sockaddr *)&sa, sizeof(sa));
    if (c->sock) {
        UTP_SetCallbacks(c->sock, &g_functable, c);
        UTP_SetSockopt(c->sock, SO_RCVBUF, RBUF_SIZE);
        UTP_Connect(c->sock);
    }
    mutexUnlock(&g_lock);

    if (!c->sock) { free_conn(c); return NULL; }

    // Wait for the handshake (state leaves CONNECTING) or timeout.
    mutexLock(&c->m);
    u64 deadline = armGetSystemTick() + armNsToTicks(ms_to_ns(timeout_ms));
    while (c->state == ST_CONNECTING) {
        u64 now = armGetSystemTick();
        if (now >= deadline) break;
        condvarWaitTimeout(&c->rcond, &c->m, armTicksToNs(deadline - now));
    }
    bool ok = (c->state == ST_CONNECTED);
    mutexUnlock(&c->m);

    if (!ok) { utp_bridge_close(c); return NULL; }
    return c;
}

int utp_bridge_read(utp_conn *c, void *buf, int len, int timeout_ms) {
    if (len <= 0) return 0;
    mutexLock(&c->m);
    u64 deadline = armGetSystemTick() + armNsToTicks(ms_to_ns(timeout_ms));
    while (c->rcount == 0 && c->state == ST_CONNECTED) {
        u64 now = armGetSystemTick();
        if (now >= deadline) break;
        condvarWaitTimeout(&c->rcond, &c->m, armTicksToNs(deadline - now));
    }
    int ret;
    if (c->rcount > 0) {
        ret = (int)rb_pop(c, (uint8_t *)buf, (size_t)len);
    } else if (c->state == ST_EOF) {
        ret = 0;
    } else {
        ret = -1;  // error or timeout
    }
    mutexUnlock(&c->m);
    return ret;
}

int utp_bridge_write(utp_conn *c, const void *buf, int len) {
    const uint8_t *p = (const uint8_t *)buf;
    int off = 0;
    while (off < len) {
        mutexLock(&c->m);
        while (wb_free(c) == 0 && c->state == ST_CONNECTED)
            condvarWaitTimeout(&c->wcond, &c->m, ms_to_ns(1000));
        if (c->state != ST_CONNECTED) { mutexUnlock(&c->m); return -1; }
        size_t space = wb_free(c);
        size_t n = (size_t)(len - off) < space ? (size_t)(len - off) : space;
        wb_push(c, p + off, n);
        off += (int)n;
        mutexUnlock(&c->m);

        mutexLock(&g_lock);
        drain_wbuf(c);
        mutexUnlock(&g_lock);
    }
    return 0;
}

void utp_bridge_close(utp_conn *c) {
    if (!c) return;
    mutexLock(&g_lock);
    c->closing = true;
    if (c->sock) UTP_Close(c->sock);  // DESTROYING (and its decref) fires later
    decref(c);                        // app releases its reference
    mutexUnlock(&g_lock);
}
