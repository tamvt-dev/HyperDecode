#include <glib.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <math.h>
#include "../include/plugin.h"
#include "../include/score.h"
#include "../include/buffer.h"

// -------------------------------------------------------------------
// Portable deterministic random (LCG) for Windows compatibility
// -------------------------------------------------------------------
static size_t lcg_random(unsigned int *state, size_t max) {
    *state = *state * 1103515245 + 12345;
    return (*state >> 16) % max;
}

// -------------------------------------------------------------------
// Core shuffle / unshuffle (binary-safe, deterministic)
// -------------------------------------------------------------------
static void shuffle_with_seed(unsigned char *data, size_t len, unsigned int seed) {
    if (len < 2) return;
    for (size_t i = len - 1; i > 0; i--) {
        unsigned int local_seed = seed + i;
        size_t j = lcg_random(&local_seed, i + 1);
        unsigned char tmp = data[i];
        data[i] = data[j];
        data[j] = tmp;
    }
}

static void unshuffle_with_seed(unsigned char *data, size_t len, unsigned int seed) {
    if (len < 2) return;
    typedef struct { size_t i; size_t j; } Swap;
    Swap *swaps = g_malloc(sizeof(Swap) * (len - 1));
    if (!swaps) return;

    size_t idx = 0;
    for (size_t i = len - 1; i > 0; i--) {
        unsigned int local_seed = seed + i;
        size_t j = lcg_random(&local_seed, i + 1);
        swaps[idx++] = (Swap){i, j};
    }

    for (size_t k = idx; k > 0; k--) {
        size_t i = swaps[k-1].i;
        size_t j = swaps[k-1].j;
        unsigned char tmp = data[i];
        data[i] = data[j];
        data[j] = tmp;
    }
    g_free(swaps);
}

// -------------------------------------------------------------------
// Encode with fixed seed 42 (string-based)
// -------------------------------------------------------------------
static char* plugin_encode(const char *input) {
    if (!input) return g_strdup("");
    size_t len = strlen(input);
    unsigned char *data = g_memdup(input, len);
    shuffle_with_seed(data, len, 42);
    char *output = g_malloc(len + 1);
    memcpy(output, data, len);
    output[len] = '\0';
    g_free(data);
    return output;
}

// -------------------------------------------------------------------
// Decode with brute-force seeds (string-based)
// -------------------------------------------------------------------
static char* plugin_decode(const char *input) {
    size_t len = strlen(input);
    const unsigned char *in = (const unsigned char*)input;

    unsigned int best_seed = 0;
    double best_score = 0.0;
    unsigned char *best_result = NULL;
    size_t best_len = 0;

    // Adaptive seed search: first 50 full, then step 5
    for (unsigned int seed = 0; seed < 200; seed++) {
        if (seed > 50 && seed % 5 != 0) continue;

        unsigned char *decoded = g_memdup(in, len);
        unshuffle_with_seed(decoded, len, seed);

        // Quick entropy reject
        double entropy = 0.0;
        int freq[256] = {0};
        for (size_t i = 0; i < len; i++) freq[decoded[i]]++;
        for (int i = 0; i < 256; i++) {
            if (freq[i]) {
                double p = (double)freq[i] / len;
                entropy -= p * log2(p);
            }
        }
        if (entropy > 6.5) {
            g_free(decoded);
            continue;
        }

        double score = score_readability(decoded, len);
        if (score > best_score) {
            if (best_result) g_free(best_result);
            best_result = decoded;
            best_len = len;
            best_seed = seed;
            best_score = score;
            if (best_score > 1.2) break; // early exit
        } else {
            g_free(decoded);
        }
    }

    if (!best_result) return g_strdup(input);

    char *output = g_malloc(best_len + 1);
    memcpy(output, best_result, best_len);
    output[best_len] = '\0';
    g_free(best_result);
    return output;
}

// -------------------------------------------------------------------
// Detection (string-based)
// -------------------------------------------------------------------
static gboolean plugin_detect(const char *input) {
    size_t len = strlen(input);
    const unsigned char *in = (const unsigned char*)input;

    for (unsigned int seed = 0; seed < 20; seed++) {
        unsigned char *decoded = g_memdup(in, len);
        unshuffle_with_seed(decoded, len, seed);
        double score = score_readability(decoded, len);
        g_free(decoded);
        if (score > 0.6) return TRUE;
    }
    return FALSE;
}

// -------------------------------------------------------------------
// Buffer-based wrappers (for plugin API)
// -------------------------------------------------------------------
static Buffer scramble_decode_buffer(Buffer in) {
    Buffer out = { NULL, NULL, 0 };
    if (!in.data || in.len == 0) return out;
    char *input_str = g_malloc(in.len + 1);
    memcpy(input_str, in.data, in.len);
    input_str[in.len] = '\0';
    char *result_str = plugin_decode(input_str);
    g_free(input_str);
    if (result_str) {
        out.data = (unsigned char*)result_str;
        out.len = strlen(result_str);
    }
    return out;
}

static Buffer scramble_encode_buffer(Buffer in) {
    Buffer out = { NULL, NULL, 0 };
    if (!in.data || in.len == 0) return out;
    char *input_str = g_malloc(in.len + 1);
    memcpy(input_str, in.data, in.len);
    input_str[in.len] = '\0';
    char *result_str = plugin_encode(input_str);
    g_free(input_str);
    if (result_str) {
        out.data = (unsigned char*)result_str;
        out.len = strlen(result_str);
    }
    return out;
}

static gboolean scramble_detect_buffer(Buffer in) {
    if (in.len == 0) return FALSE;
    char *input_str = g_malloc(in.len + 1);
    memcpy(input_str, in.data, in.len);
    input_str[in.len] = '\0';
    gboolean res = plugin_detect(input_str);
    g_free(input_str);
    return res;
}

void scramble_plugin_init(void) {
    g_plugin_registry->register_plugin("Scramble",
                                        scramble_decode_buffer,
                                        NULL,
                                        scramble_encode_buffer,
                                        scramble_detect_buffer,
                                        60);
}