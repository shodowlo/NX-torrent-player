#include "bencode.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *buf;
    size_t len;
    size_t pos;
} be_parser;

static be_node *parse_value(be_parser *p);

static int peek(be_parser *p) {
    return p->pos < p->len ? (unsigned char)p->buf[p->pos] : -1;
}

static be_node *node_new(be_type type) {
    be_node *n = calloc(1, sizeof(be_node));
    if (n) n->type = type;
    return n;
}

// Parses "<len>:<bytes>" and returns pointer/length into the buffer.
static int parse_rawstr(be_parser *p, const char **out, size_t *outlen) {
    size_t slen = 0;
    if (peek(p) < '0' || peek(p) > '9') return -1;
    while (peek(p) >= '0' && peek(p) <= '9') {
        slen = slen * 10 + (p->buf[p->pos] - '0');
        if (slen > p->len) return -1;
        p->pos++;
    }
    if (peek(p) != ':') return -1;
    p->pos++;
    if (p->pos + slen > p->len) return -1;
    *out = p->buf + p->pos;
    *outlen = slen;
    p->pos += slen;
    return 0;
}

static be_node *parse_int(be_parser *p) {
    p->pos++; // skip 'i'
    int neg = 0;
    if (peek(p) == '-') { neg = 1; p->pos++; }
    if (peek(p) < '0' || peek(p) > '9') return NULL;
    int64_t v = 0;
    while (peek(p) >= '0' && peek(p) <= '9') {
        v = v * 10 + (p->buf[p->pos] - '0');
        p->pos++;
    }
    if (peek(p) != 'e') return NULL;
    p->pos++;
    be_node *n = node_new(BE_INT);
    if (n) n->i = neg ? -v : v;
    return n;
}

static be_node *parse_list(be_parser *p) {
    p->pos++; // skip 'l'
    be_node *n = node_new(BE_LIST);
    if (!n) return NULL;
    while (peek(p) != 'e') {
        if (peek(p) < 0) { be_free(n); return NULL; }
        be_node *item = parse_value(p);
        if (!item) { be_free(n); return NULL; }
        be_node **grown = realloc(n->list.items, (n->list.count + 1) * sizeof(be_node *));
        if (!grown) { be_free(item); be_free(n); return NULL; }
        n->list.items = grown;
        n->list.items[n->list.count++] = item;
    }
    p->pos++; // skip 'e'
    return n;
}

static be_node *parse_dict(be_parser *p) {
    p->pos++; // skip 'd'
    be_node *n = node_new(BE_DICT);
    if (!n) return NULL;
    while (peek(p) != 'e') {
        const char *key;
        size_t keylen;
        if (parse_rawstr(p, &key, &keylen) != 0) { be_free(n); return NULL; }
        be_node *val = parse_value(p);
        if (!val) { be_free(n); return NULL; }
        be_dict_entry *grown = realloc(n->dict.items, (n->dict.count + 1) * sizeof(be_dict_entry));
        if (!grown) { be_free(val); be_free(n); return NULL; }
        n->dict.items = grown;
        n->dict.items[n->dict.count].key = key;
        n->dict.items[n->dict.count].keylen = keylen;
        n->dict.items[n->dict.count].val = val;
        n->dict.count++;
    }
    p->pos++; // skip 'e'
    return n;
}

static be_node *parse_value(be_parser *p) {
    size_t start = p->pos;
    be_node *n;
    switch (peek(p)) {
        case 'i': n = parse_int(p); break;
        case 'l': n = parse_list(p); break;
        case 'd': n = parse_dict(p); break;
        default: {
            n = node_new(BE_STR);
            if (!n) return NULL;
            if (parse_rawstr(p, &n->str.ptr, &n->str.len) != 0) {
                free(n);
                return NULL;
            }
            break;
        }
    }
    if (n) {
        n->raw = p->buf + start;
        n->rawlen = p->pos - start;
    }
    return n;
}

be_node *be_parse(const char *buf, size_t len) {
    be_parser p = { buf, len, 0 };
    return parse_value(&p);
}

void be_free(be_node *n) {
    if (!n) return;
    if (n->type == BE_LIST) {
        for (size_t i = 0; i < n->list.count; i++)
            be_free(n->list.items[i]);
        free(n->list.items);
    } else if (n->type == BE_DICT) {
        for (size_t i = 0; i < n->dict.count; i++)
            be_free(n->dict.items[i].val);
        free(n->dict.items);
    }
    free(n);
}

be_node *be_dict_get(const be_node *n, const char *key) {
    if (!n || n->type != BE_DICT) return NULL;
    size_t keylen = strlen(key);
    for (size_t i = 0; i < n->dict.count; i++) {
        if (n->dict.items[i].keylen == keylen &&
            memcmp(n->dict.items[i].key, key, keylen) == 0)
            return n->dict.items[i].val;
    }
    return NULL;
}
