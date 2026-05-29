#include <glib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "../include/plugin.h"
#include "../include/score.h"
#include "../include/buffer.h"

// -------------------------------------------------------------------
// Core XOR logic (Buffer-based)
// -------------------------------------------------------------------
static Buffer xor_transform(Buffer in, unsigned char key) {
    Buffer out = { NULL, NULL, 0 };
    if (!in.data || in.len == 0) return out;
    
    unsigned char *res = g_malloc(in.len);
    for (size_t i = 0; i < in.len; i++) {
        res[i] = in.data[i] ^ key;
    }
    
    out.data = res;
    out.len = in.len;
    return out;
}

// -------------------------------------------------------------------
// Candidate management for brute-force
// -------------------------------------------------------------------
typedef struct {
    Buffer buf;
    double score;
    int key;
} XorCandidate;

static int cmp_xor_candidate(const void *a, const void *b) {
    const XorCandidate *ca = (const XorCandidate*)a;
    const XorCandidate *cb = (const XorCandidate*)b;
    if (cb->score > ca->score) return 1;
    if (cb->score < ca->score) return -1;
    return 0;
}

// -------------------------------------------------------------------
// CyberChef-style Magic: 1-byte XOR Brute Force
// -------------------------------------------------------------------
static GList* xor_decode_multi(Buffer in) {
    if (!in.data || in.len == 0) return NULL;

    XorCandidate all_cands[256];
    int valid_count = 0;

    // Phase 1: FAST sampling 256 keys without full allocation
    unsigned char sample[256];
    size_t sample_len = MIN(in.len, 256);
    
    for (int key = 0; key < 256; key++) {
        // Create XOR sample in stack memory
        for (size_t i = 0; i < sample_len; i++) {
            sample[i] = in.data[i] ^ (unsigned char)key;
        }
        
        // Fast score check on sample
        double score = score_readability(sample, sample_len);
        
        if (score > 0.45) { // Slightly higher threshold for samples
            // Phase 2: High-potential key, perform FULL transform and allocation
            Buffer result = xor_transform(in, (unsigned char)key);
            all_cands[valid_count].buf = result;
            all_cands[valid_count].score = score;
            all_cands[valid_count].key = key;
            valid_count++;
        }
    }

    if (valid_count == 0) return NULL;

    // Sort candidates by score descending
    qsort(all_cands, valid_count, sizeof(XorCandidate), cmp_xor_candidate);

    GList *results = NULL;
    // Keep top 5 promising results to feed into the pipeline
    int keep = MIN(valid_count, 5);
    for (int i = 0; i < keep; i++) {
        Buffer *boxed = g_new0(Buffer, 1);
        *boxed = all_cands[i].buf;
        results = g_list_append(results, boxed);
    }

    // Free losers
    for (int i = keep; i < valid_count; i++) {
        buffer_free(&all_cands[i].buf);
    }

    return results;
}

static gboolean xor_detect(Buffer in) {
    if (in.len < 4) return FALSE;
    
    // Quick heuristic: sample common XOR keys for text
    const unsigned char test_keys[] = {0x00, 0x20, 0xFF, 0x55, 0xAA};
    for (int i = 0; i < 5; i++) {
        Buffer res = xor_transform(in, test_keys[i]);
        double s = score_readability(res.data, MIN(res.len, 128));
        buffer_free(&res);
        if (s > 0.7) return TRUE;
    }
    
    // If it's a small buffer and looks random, give XOR a chance anyway
    if (in.len < 100) return TRUE; 
    
    return FALSE;
}

static Buffer xor_decode_single(Buffer in) {
    // Single decode uses the best found key from a quick scan
    GList *multi = xor_decode_multi(in);
    if (!multi) return (Buffer){NULL, NULL, 0};
    
    Buffer *best = (Buffer*)multi->data;
    Buffer out = buffer_clone(best);
    
    // Clean up
    for (GList *iter = multi; iter; iter = iter->next) {
        buffer_free((Buffer*)iter->data);
        g_free(iter->data);
    }
    g_list_free(multi);
    
    return out;
}

void xor_plugin_init(void) {
    g_plugin_registry->register_plugin("XOR",
                                       xor_decode_single,
                                       xor_decode_multi,
                                       NULL, // Encode is same as decode
                                       xor_detect,
                                       40); // Lower base priority, high detect bonus
}