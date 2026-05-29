#include <glib.h>
#include <string.h>
#include <ctype.h>
#include "../include/plugin.h"
#include "../include/score.h"
#include "../include/buffer.h"

// --- Buffer-based core logic ---
static Buffer caesar_apply_shift(Buffer in, int shift) {
    Buffer out = { NULL, NULL, 0 };
    if (!in.data || in.len == 0) return out;

    unsigned char *res = g_malloc(in.len);
    for (size_t i = 0; i < in.len; i++) {
        unsigned char c = in.data[i];
        if (c >= 'a' && c <= 'z')
            c = ((c - 'a' - shift + 26) % 26) + 'a';
        else if (c >= 'A' && c <= 'Z')
            c = ((c - 'A' - shift + 26) % 26) + 'A';
        res[i] = c;
    }

    out.data = res;
    out.len = in.len;
    return out;
}

static GList* caesar_decode_multi(Buffer in) {
    GList *results = NULL;
    if (!in.data || in.len == 0) return NULL;

    for (int s = 1; s <= 25; s++) {
        Buffer out = caesar_apply_shift(in, s);
        
        // Quality filter: only keep results that look somewhat printable/readable
        // to avoid polluting the beam with 25 variants of garbage.
        if (score_readability(out.data, MIN(out.len, 128)) > 0.4) {
            Buffer *boxed = g_new0(Buffer, 1);
            *boxed = out;
            results = g_list_append(results, boxed);
        } else {
            buffer_free(&out);
        }
    }
    return results;
}

static gboolean caesar_detect_buffer(Buffer in) {
    if (in.len < 4) return FALSE;

    // Detection is broad: if there's significant alpha content, we try Caesar.
    // The scoring engine will handle the fine-grained ranking.
    int alpha = 0;
    size_t sample = MIN(in.len, 128);
    for (size_t i = 0; i < sample; i++) {
        if (isalpha(in.data[i])) alpha++;
    }

    return alpha > (int)(sample * 0.5);
}

// Single decode wrapper (for compatibility, usually unused by multi-plugins)
static Buffer caesar_decode_buffer(Buffer in) {
    return caesar_apply_shift(in, 3);
}

static Buffer caesar_encode_buffer(Buffer in) {
    // Encoding is just decoding with shift = 26 - shift
    return caesar_apply_shift(in, 26 - 3);
}

void caesar_plugin_init(void) {
    g_plugin_registry->register_plugin("Caesar",
                                        caesar_decode_buffer,
                                        caesar_decode_multi,
                                        caesar_encode_buffer,
                                        caesar_detect_buffer,
                                        30); // Exploratory priority
}