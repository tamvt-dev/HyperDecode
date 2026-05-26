#include "../include/buffer.h"
#include <glib.h>
#include <string.h>

#define XXH_INLINE_ALL
#include "third_party/xxhash.h"

Buffer buffer_new(const unsigned char *data, size_t len) {
    Buffer buf = { NULL, NULL, 0 };
    if (!data || len == 0) return buf;
    
    SharedBuffer *shared = (SharedBuffer*)g_malloc(sizeof(SharedBuffer) + len);
    shared->ref_count = 1;
    shared->capacity = len;
    memcpy(shared->data, data, len);
    
    buf.shared = shared;
    buf.data = shared->data;
    buf.len = len;
    return buf;
}

Buffer buffer_clone(const Buffer *src) {
    if (!src || !src->shared || !src->data) return (Buffer){ NULL, NULL, 0 };
    g_atomic_int_inc(&src->shared->ref_count);
    return *src;
}

void buffer_free(Buffer *buf) {
    if (buf && buf->shared) {
        if (g_atomic_int_dec_and_test(&buf->shared->ref_count)) {
            g_free(buf->shared);
        }
        buf->shared = NULL;
        buf->data = NULL;
        buf->len = 0;
    }
}

gboolean buffer_equal(const Buffer *a, const Buffer *b) {
    if (!a || !b) return FALSE;
    if (a->data == b->data && a->len == b->len) return TRUE; // Fast path
    if (a->len != b->len) return FALSE;
    if (a->len == 0) return TRUE;
    return memcmp(a->data, b->data, a->len) == 0;
}

guint64 buffer_hash_fast(const Buffer *buf) {
    if (!buf || !buf->data || buf->len == 0) return 0;
    return XXH64(buf->data, buf->len, 0);
}

char* buffer_key(const Buffer *buf) {
    if (!buf || !buf->data || buf->len == 0) return g_strdup("");

    static const char hex_table[] = "0123456789abcdef";
    size_t len = buf->len;
    unsigned char *data = buf->data;
    
    // Optimization: limit the key length for extremely large buffers?
    // Maybe not, but let's keep it robust for the pipeline.
    char *out = (char*)g_malloc(len * 2 + 1);
    for (size_t i = 0; i < len; i++) {
        out[i*2]     = hex_table[(data[i] >> 4) & 0xF];
        out[i*2 + 1] = hex_table[data[i] & 0xF];
    }
    out[len * 2] = '\0';
    return out;
}