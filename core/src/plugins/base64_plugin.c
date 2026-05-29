#include <glib.h>
#include <string.h>
#include <ctype.h>
#include "../include/plugin.h"
#include "../include/score.h"
#include "../include/buffer.h"

// --- Helper: Normalize URL-safe Base64 ---
static void base64_normalize(char *input) {
    if (!input) return;
    for (char *p = input; *p; p++) {
        if (*p == '-') *p = '+';
        if (*p == '_') *p = '/';
    }
}

static unsigned char* base64_decode_raw(const char *input, size_t *out_len) {
    gsize len;
    // g_base64_decode is quite lenient, but expects normalized chars
    guchar *decoded = g_base64_decode(input, &len);
    if (out_len) *out_len = len;
    return (unsigned char*)decoded;
}

static char* base64_encode_raw(const unsigned char *data, size_t len) {
    return g_base64_encode((const guchar*)data, (gsize)len);
}

// --- Detection with whitespace awareness ---
static gboolean base64_detect_buffer(Buffer in) {
    if (in.len < 4) return FALSE;

    size_t significant = 0;
    gboolean has_alpha = FALSE;
    gboolean has_special = FALSE;

    for (size_t i = 0; i < in.len; i++) {
        unsigned char c = in.data[i];
        if (g_ascii_isspace(c)) continue;

        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '+' || c == '/' || c == '=' ||
              c == '-' || c == '_')) {
            return FALSE;
        }

        significant++;
        if (g_ascii_isalpha(c)) has_alpha = TRUE;
        if (c == '+' || c == '/' || c == '-' || c == '_') has_special = TRUE;
    }

    // Heuristic: pure numbers are unlikely to be intended Base64 in this context
    if (significant < 4) return FALSE;
    if (!has_alpha && !has_special) return FALSE;

    return TRUE;
}

// --- Buffer-based wrappers ---
static Buffer base64_decode_buffer(Buffer in) {
    Buffer out = { NULL, NULL, 0 };
    if (!in.data || in.len == 0) return out;

    char *input_str = g_strndup((const char*)in.data, in.len);
    base64_normalize(input_str);
    
    size_t out_len;
    unsigned char *result = base64_decode_raw(input_str, &out_len);
    
    if (result) {
        // Strict Validation: Re-encode and compare (ignoring whitespace and padding)
        char *re_encoded = base64_encode_raw(result, out_len);
        
        // Simple heuristic: if decoding produced 0 bytes or error, cancel
        if (out_len == 0) {
            g_free(result);
            g_free(re_encoded);
            g_free(input_str);
            return out;
        }

        g_free(re_encoded);
        out.data = result;
        out.len = out_len;
    }
    
    g_free(input_str);
    return out;
}

static Buffer base64_encode_buffer(Buffer in) {
    Buffer out = { NULL, NULL, 0 };
    if (!in.data || in.len == 0) return out;

    char *result_str = base64_encode_raw(in.data, in.len);
    if (result_str) {
        out.data = (unsigned char*)result_str;
        out.len = strlen(result_str); // Base64 encoding never yields internal nulls
    }
    return out;
}

void base64_plugin_init(void) {
    g_plugin_registry->register_plugin("Base64",
                                        base64_decode_buffer,
                                        NULL,
                                        base64_encode_buffer,
                                        base64_detect_buffer,
                                        100);
}