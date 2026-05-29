#include <glib.h>
#include <string.h>
#include <ctype.h>
#include "../include/plugin.h"
#include "../include/score.h"
#include "../include/buffer.h"

// --- Performance: Buffer-based In-place Style Decoder ---
static Buffer url_decode_buffer(Buffer in) {
    Buffer out = { NULL, NULL, 0 };
    if (!in.data || in.len == 0) return out;

    unsigned char *res = g_malloc(in.len);
    size_t j = 0;

    for (size_t i = 0; i < in.len; i++) {
        unsigned char c = in.data[i];

        // Defensive decode: only process %XX if XX are valid hex digits
        if (c == '%' && i + 2 < in.len &&
            g_ascii_isxdigit(in.data[i+1]) &&
            g_ascii_isxdigit(in.data[i+2])) {

            char hex[3] = { (char)in.data[i+1], (char)in.data[i+2], 0 };
            res[j++] = (unsigned char)g_ascii_strtoull(hex, NULL, 16);
            i += 2;

        } else if (c == '+') {
            res[j++] = ' ';
        } else {
            res[j++] = c;
        }
    }

    // Binary safety: check for trash output
    // If the decode result is mostly binary garbage, it's likely a false positive
    size_t printable = 0;
    for (size_t k = 0; k < j; k++) {
        if (g_ascii_isprint(res[k]) || g_ascii_isspace(res[k])) printable++;
    }

    if (j > 0 && (double)printable / j < 0.4) {
        g_free(res);
        return out;
    }

    out.data = res;
    out.len = j;
    return out;
}

static Buffer url_encode_buffer(Buffer in) {
    Buffer out = { NULL, NULL, 0 };
    if (!in.data || in.len == 0) return out;

    GString *result = g_string_sized_new(in.len * 3);
    for (size_t i = 0; i < in.len; i++) {
        unsigned char c = in.data[i];
        if (g_ascii_isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            g_string_append_c(result, (char)c);
        } else if (c == ' ') {
            g_string_append_c(result, '+');
        } else {
            g_string_append_printf(result, "%%%02X", c);
        }
    }

    out.len = result->len;
    out.data = (unsigned char*)g_string_free(result, FALSE);
    return out;
}

static gboolean url_detect_buffer(Buffer in) {
    if (in.len < 3) return FALSE;

    // Improved detection: look for valid %XX patterns
    int percent_count = 0;
    size_t sample = MIN(in.len, 512);

    for (size_t i = 0; i + 2 < sample; i++) {
        if (in.data[i] == '%' &&
            g_ascii_isxdigit(in.data[i+1]) &&
            g_ascii_isxdigit(in.data[i+2])) {
            percent_count++;
        }
    }

    // Heuristic: at least one valid sequence or plus signs in a text-like buffer
    return percent_count >= 1;
}

void url_plugin_init(void) {
    g_plugin_registry->register_plugin("URL",
                                        url_decode_buffer,
                                        NULL,
                                        url_encode_buffer,
                                        url_detect_buffer,
                                        90);
}