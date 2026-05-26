#ifndef BUFFER_H
#define BUFFER_H
#include <glib.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SharedBuffer {
    gint ref_count;
    size_t capacity;
    unsigned char data[];
} SharedBuffer;

typedef struct {
    SharedBuffer *shared;
    unsigned char *data;
    size_t len;
} Buffer;

Buffer buffer_new(const unsigned char *data, size_t len);
Buffer buffer_clone(const Buffer *src);
void buffer_free(Buffer *buf);
gboolean buffer_equal(const Buffer *a, const Buffer *b);
char* buffer_key(const Buffer *buf);

// Fast 64-bit hash for deduplication
guint64 buffer_hash_fast(const Buffer *buf);

#ifdef __cplusplus
}
#endif
#endif // BUFFER_H