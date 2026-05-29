#include <glib.h>
#include <string.h>
#include <ctype.h>
#include "../include/plugin.h"
#include "../include/score.h"
#include "../include/buffer.h"

static char* rot13_decode_str(const char *input) {
    if (!input) return g_strdup("");
    GString *result = g_string_sized_new(strlen(input));
    for (size_t i = 0; i < strlen(input); i++) {
        char c = input[i];
        if (c >= 'a' && c <= 'z')
            c = ((c - 'a' + 13) % 26) + 'a';
        else if (c >= 'A' && c <= 'Z')
            c = ((c - 'A' + 13) % 26) + 'A';
        g_string_append_c(result, c);
    }
    return g_string_free(result, FALSE);
}

static char* rot13_encode_str(const char *input) {
    return rot13_decode_str(input);
}

static gboolean rot13_detect_str(const char *input) {
    if (!input || strlen(input) < 3) return FALSE;
    char *decoded = rot13_decode_str(input);
    int letter_count = 0;
    for (size_t i = 0; i < strlen(decoded) && i < 100; i++) {
        if (isalpha(decoded[i])) letter_count++;
    }
    gboolean result = (letter_count > strlen(decoded) * 0.3);
    g_free(decoded);
    return result;
}

static Buffer rot13_decode_buffer(Buffer in) {
    Buffer out = { NULL, NULL, 0 };
    if (!in.data || in.len == 0) return out;
    char *input_str = g_malloc(in.len + 1);
    memcpy(input_str, in.data, in.len);
    input_str[in.len] = '\0';
    char *result_str = rot13_decode_str(input_str);
    g_free(input_str);
    if (result_str) {
        out.data = (unsigned char*)result_str;
        out.len = strlen(result_str);
    }
    return out;
}

static Buffer rot13_encode_buffer(Buffer in) {
    Buffer out = { NULL, NULL, 0 };
    if (!in.data || in.len == 0) return out;
    char *input_str = g_malloc(in.len + 1);
    memcpy(input_str, in.data, in.len);
    input_str[in.len] = '\0';
    char *result_str = rot13_encode_str(input_str);
    g_free(input_str);
    if (result_str) {
        out.data = (unsigned char*)result_str;
        out.len = strlen(result_str);
    }
    return out;
}

static gboolean rot13_detect_buffer(Buffer in) {
    if (in.len == 0) return FALSE;
    char *input_str = g_malloc(in.len + 1);
    memcpy(input_str, in.data, in.len);
    input_str[in.len] = '\0';
    gboolean res = rot13_detect_str(input_str);
    g_free(input_str);
    return res;
}

void rot13_plugin_init(void) {
    g_plugin_registry->register_plugin("ROT13",
                                        rot13_decode_buffer,
                                        NULL,
                                        rot13_encode_buffer,
                                        rot13_detect_buffer,
                                        80);
}