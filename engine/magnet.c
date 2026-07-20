#include "magnet.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *err, size_t errlen, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s", msg);
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// In-place percent-decode (magnet query values are URL-encoded).
static void url_decode(char *s) {
    char *o = s;
    while (*s) {
        if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
            *o++ = (char)((hexval(s[1]) << 4) | hexval(s[2]));
            s += 3;
        } else if (*s == '+') {
            *o++ = ' ';
            s++;
        } else {
            *o++ = *s++;
        }
    }
    *o = '\0';
}

static int decode_hex_hash(const char *hex, uint8_t out[20]) {
    for (int i = 0; i < 20; i++) {
        int hi = hexval(hex[i * 2]), lo = hexval(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

// RFC 4648 base32 decode of a 32-char string into 20 bytes.
static int decode_base32_hash(const char *in, uint8_t out[20]) {
    static const char *A = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    uint64_t buf = 0;
    int bits = 0, o = 0;
    for (int i = 0; i < 32; i++) {
        const char *p = strchr(A, toupper((unsigned char)in[i]));
        if (!p) return -1;
        buf = (buf << 5) | (uint64_t)(p - A);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)((buf >> bits) & 0xFF);
        }
    }
    return o == 20 ? 0 : -1;
}

static void add_tracker(magnet_info *m, const char *url) {
    if (m->tracker_count >= MAX_TRACKERS) return;
    int is_http = strncmp(url, "http", 4) == 0;
    int is_udp = strncmp(url, "udp://", 6) == 0;
    if (!is_http && !is_udp) return;  // wss/other not supported
    char *copy = strdup(url);
    if (copy) m->trackers[m->tracker_count++] = copy;
}

int magnet_parse(const char *uri, magnet_info *m, char *err, size_t errlen) {
    memset(m, 0, sizeof(*m));

    if (strncmp(uri, "magnet:?", 8) != 0) {
        set_err(err, errlen, "pas un lien magnet");
        return -1;
    }

    bool have_hash = false;
    char *query = strdup(uri + 8);
    if (!query) { set_err(err, errlen, "out of memory"); return -1; }

    // Walk &-separated key=value pairs.
    char *save = NULL;
    for (char *tok = strtok_r(query, "&", &save); tok; tok = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = tok;
        char *val = eq + 1;
        url_decode(val);

        if (strcmp(key, "xt") == 0) {
            // xt=urn:btih:<40 hex | 32 base32>
            const char *pfx = "urn:btih:";
            if (strncmp(val, pfx, strlen(pfx)) == 0) {
                const char *h = val + strlen(pfx);
                size_t hlen = strlen(h);
                if (hlen == 40 && decode_hex_hash(h, m->info_hash) == 0) have_hash = true;
                else if (hlen == 32 && decode_base32_hash(h, m->info_hash) == 0) have_hash = true;
            }
        } else if (strcmp(key, "dn") == 0) {
            snprintf(m->name, sizeof(m->name), "%s", val);
        } else if (strcmp(key, "tr") == 0) {
            add_tracker(m, val);
        }
    }
    free(query);

    if (!have_hash) {
        set_err(err, errlen, "magnet without a valid info hash");
        magnet_free(m);
        return -1;
    }
    return 0;
}

void magnet_free(magnet_info *m) {
    for (int i = 0; i < m->tracker_count; i++)
        free(m->trackers[i]);
    m->tracker_count = 0;
}
