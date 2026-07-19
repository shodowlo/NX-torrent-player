#ifndef BENCODE_H
#define BENCODE_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    BE_INT,
    BE_STR,
    BE_LIST,
    BE_DICT,
} be_type;

typedef struct be_node be_node;

typedef struct {
    const char *key;
    size_t keylen;
    be_node *val;
} be_dict_entry;

struct be_node {
    be_type type;
    // Raw span in the source buffer (needed to hash the info dict verbatim)
    const char *raw;
    size_t rawlen;
    union {
        int64_t i;
        struct { const char *ptr; size_t len; } str;
        struct { be_node **items; size_t count; } list;
        struct { be_dict_entry *items; size_t count; } dict;
    };
};

// Parse one bencode value. Strings point into buf (no copy) — keep buf alive.
be_node *be_parse(const char *buf, size_t len);
void be_free(be_node *n);

// Dict lookup by NUL-terminated key. NULL if absent or n is not a dict.
be_node *be_dict_get(const be_node *n, const char *key);

#endif
