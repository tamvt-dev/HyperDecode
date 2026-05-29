#include <glib.h>
#include <string.h>
#include <ctype.h>
#include "../include/plugin.h"
#include "../include/score.h"
#include "../include/buffer.h"

// --- Buffer-based core logic ---
static Buffer atbash_transform(Buffer in) {
    Buffer out = { NULL, NULL, 0 };
    if (!in.data || in.len == 0) return out;

    unsigned char *res = g_malloc(in.len);
    for (size_t i = 0; i < in.len; i++) {
        unsigned char c = in.data[i];
        if (c >= 'a' && c <= 'z')
            c = 'z' - (c - 'a');
        else if (c >= 'A' && c <= 'Z')
            c = 'Z' - (c - 'A');
        res[i] = c;
    }

    out.data = res;
    out.len = in.len;
    return out;
}

static gboolean atbash_detect_buffer(Buffer in) {
    if (in.len < 3) return FALSE;

    // Fast check: Atbash target should result in "readable" text.
    // We decode a small sample if needed, or just use the plugin 
    // to transform and score the result.
    Buffer decoded = atbash_transform(in);
    double score = score_readability(decoded.data, MIN(decoded.len, 256));
    
    // Heuristic: Atbash input variant is useful if the TRANSFORMED result 
    // is significantly more readable than the input, or passes a base threshold.
    // However, Atbash is self-inverse, so just checking the transformed score is best.
    gboolean is_promising = (score > 0.65);
    
    buffer_free(&decoded);
    return is_promising;
}

static Buffer atbash_decode_buffer(Buffer in) {
    return atbash_transform(in);
}

static Buffer atbash_encode_buffer(Buffer in) {
    return atbash_transform(in);
}

void atbash_plugin_init(void) {
    g_plugin_registry->register_plugin("Atbash",
                                        atbash_decode_buffer,
                                        NULL,                     // no multi-output
                                        atbash_encode_buffer,
                                        atbash_detect_buffer,
                                        35); // Lower base priority for exploratory transform
}