#include "../include/core.h"
#include "../include/decoder.h"
#include "../include/encoder.h"
#include "../include/plugin.h"
#include "../include/lru_cache.h"
#include "../include/logging.h"
#include "../include/constants.h"
#include "../include/version.h"
#include "../include/buffer.h"
#include "../include/score.h"
#include <string.h>

extern void base64_plugin_init(void);
extern void rot13_plugin_init(void);
extern void url_plugin_init(void);
extern void atbash_plugin_init(void);
extern void caesar_plugin_init(void);
extern void xor_plugin_init(void);
extern void scramble_plugin_init(void);
extern void aes_plugin_init(void);
extern void gzip_plugin_init(void);
extern void sha256_plugin_init(void);

typedef struct {
    LRUCache *cache;
    PluginManager *plugin_manager;
    CoreStats stats;
    GMutex stats_mutex;
    GMutex cache_mutex;
    gboolean initialized;
} CoreContext;

static CoreContext *g_core = NULL;
static gsize core_init_once = 0;

static gboolean looks_structured_binary(const char *input, size_t len) {
    return input && len > 0 && is_binary(input, len);
}

static gboolean looks_structured_hex(const char *input, size_t len) {
    return input && len > 0 && is_hex(input, len);
}

static gboolean looks_structured_base64(const char *input, size_t len) {
    return input && len > 0 && is_base64(input, len);
}

static gboolean looks_structured_morse(const char *input, size_t len) {
    return input && len > 0 && is_morse(input, len);
}

static DecodeResult* build_decode_result_from_output(const char *input, const char *format, char *output) {
    DecodeResult *result = g_new0(DecodeResult, 1);
    result->input = g_strdup(input);
    result->format = g_strdup(format);
    result->output = output ? output : g_strdup("");

    if (output && output[0] != '\0') {
        result->error_code = ERROR_OK;
        result->output_size = strlen(output);
    } else {
        result->error_code = ERROR_DECODE_FAILED;
        result->error_message = g_strdup("Decode failed");
    }

    return result;
}

static DecodeResult* decode_with_best_plugin_candidate(const char *input) {
    if (!g_core || !g_core->plugin_manager || !input || !*input) {
        return NULL;
    }

    Buffer in_buf = buffer_new((const unsigned char*)input, strlen(input));
    if (!in_buf.data) {
        return NULL;
    }

    GList *plugins = plugin_manager_list(g_core->plugin_manager);
    DecodeResult *best = NULL;
    double best_score = -1.0;

    for (GList *iter = plugins; iter; iter = iter->next) {
        Plugin *p = (Plugin*)iter->data;
        if (!p || !p->enabled || !p->decode_single || !p->detect) {
            continue;
        }
        if (!p->detect(in_buf)) {
            continue;
        }

        Buffer out_buf = p->decode_single(in_buf);
        if (!out_buf.data || out_buf.len == 0) {
            buffer_free(&out_buf);
            continue;
        }

        double readability = score_readability(out_buf.data, out_buf.len);
        if (readability > best_score) {
            if (best) {
                decode_result_free(best);
            }

            char *output = g_malloc(out_buf.len + 1);
            memcpy(output, out_buf.data, out_buf.len);
            output[out_buf.len] = '\0';

            best = build_decode_result_from_output(input, p->name, output);
            best_score = readability;
        }

        buffer_free(&out_buf);
    }

    g_list_free(plugins);
    buffer_free(&in_buf);
    return best;
}

static void core_init_once_func(void) {
    log_info("Initializing Core v%s", APP_VERSION);

    g_core = g_new0(CoreContext, 1);
    g_core->cache = lru_cache_new(CACHE_SIZE_DEFAULT);
    g_core->plugin_manager = plugin_manager_new();
    g_mutex_init(&g_core->stats_mutex);
    g_mutex_init(&g_core->cache_mutex);

    g_core->initialized = TRUE;
    log_info("Core initialized");

    base64_plugin_init();
    rot13_plugin_init();
    url_plugin_init();
    atbash_plugin_init();
    caesar_plugin_init();
    xor_plugin_init();
    scramble_plugin_init();
    aes_plugin_init();
    gzip_plugin_init();
    sha256_plugin_init();
}

gboolean core_init(void) {
    if (g_once_init_enter(&core_init_once)) {
        core_init_once_func();
        g_once_init_leave(&core_init_once, 1);
    }
    return g_core && g_core->initialized;
}

void core_cleanup(void) {
    if (g_core) {
        log_info("Cleaning up core...");
        if (g_core->cache) {
            lru_cache_free(g_core->cache);
        }
        if (g_core->plugin_manager) {
            plugin_manager_free(g_core->plugin_manager);
        }
        g_mutex_clear(&g_core->stats_mutex);
        g_mutex_clear(&g_core->cache_mutex);
        g_free(g_core);
        g_core = NULL;
        log_info("Core cleanup complete");
    }
}

static DecodeResult* decode_builtin(const char *input) {
    size_t len = strlen(input);

    if (looks_structured_binary(input, len)) {
        return build_decode_result_from_output(input, "Binary", decode_binary(input));
    }
    if (looks_structured_hex(input, len)) {
        return build_decode_result_from_output(input, "Hex", decode_hex(input));
    }
    if (looks_structured_morse(input, len)) {
        return build_decode_result_from_output(input, "Morse", decode_morse(input));
    }
    if (looks_structured_base64(input, len)) {
        return build_decode_result_from_output(input, "Base64", decode_base64(input));
    }

    DecodeResult *result = g_new0(DecodeResult, 1);
    result->input = g_strdup(input);
    result->error_code = ERROR_UNKNOWN_FORMAT;
    result->error_message = g_strdup("Cannot detect format");
    result->output = g_strdup("");
    result->format = g_strdup("Unknown");
    return result;
}

DecodeResult* core_decode(const char *input) {
    if (!core_init()) {
        DecodeResult *result = g_new0(DecodeResult, 1);
        result->error_code = ERROR_GENERIC;
        result->error_message = g_strdup("Core not initialized");
        return result;
    }

    if (!input || strlen(input) == 0) {
        DecodeResult *result = g_new0(DecodeResult, 1);
        result->error_code = ERROR_INVALID_INPUT;
        result->error_message = g_strdup("Empty input");
        return result;
    }

    GTimer *timer = g_timer_new();

    g_mutex_lock(&g_core->cache_mutex);
    char *cached = lru_cache_get(g_core->cache, input);
    g_mutex_unlock(&g_core->cache_mutex);

    if (cached) {
        g_timer_stop(timer);
        DecodeResult *result = g_new0(DecodeResult, 1);
        result->output = g_strdup(cached);
        result->format = g_strdup("Cached");
        result->from_cache = TRUE;
        result->processing_time_ms = g_timer_elapsed(timer, NULL) * 1000;
        result->input_size = strlen(input);
        result->output_size = strlen(cached);
        g_timer_destroy(timer);

        g_mutex_lock(&g_core->stats_mutex);
        g_core->stats.total_decodes++;
        g_core->stats.cache_hits++;
        g_mutex_unlock(&g_core->stats_mutex);
        return result;
    }

    DecodeResult *result = NULL;
    const size_t input_len = strlen(input);

    if (looks_structured_binary(input, input_len) ||
        looks_structured_hex(input, input_len) ||
        looks_structured_morse(input, input_len) ||
        looks_structured_base64(input, input_len)) {
        result = decode_builtin(input);
    } else {
        result = decode_with_best_plugin_candidate(input);
        if (!result || result->error_code != ERROR_OK) {
            if (result) {
                decode_result_free(result);
            }
            result = decode_builtin(input);
        }
    }

    g_timer_stop(timer);

    if (result && result->error_code == ERROR_OK) {
        result->processing_time_ms = g_timer_elapsed(timer, NULL) * 1000;
        result->input_size = strlen(input);
        result->output_size = result->output ? strlen(result->output) : 0;

        g_mutex_lock(&g_core->cache_mutex);
        lru_cache_put(g_core->cache, input, result->output);
        g_mutex_unlock(&g_core->cache_mutex);
    }

    g_timer_destroy(timer);

    g_mutex_lock(&g_core->stats_mutex);
    g_core->stats.total_decodes++;
    if (result && result->error_code == ERROR_OK) {
        g_core->stats.total_input_bytes += result->input_size;
        g_core->stats.total_output_bytes += result->output_size;
        g_core->stats.avg_decode_time_ms =
            (g_core->stats.avg_decode_time_ms * (g_core->stats.total_decodes - 1) +
             result->processing_time_ms) / g_core->stats.total_decodes;
    }
    g_mutex_unlock(&g_core->stats_mutex);

    return result;
}

DecodeResult* core_decode_recursive(const char *input, int max_depth) {
    if (max_depth <= 0 || max_depth > MAX_DECODE_DEPTH) {
        return core_decode(input);
    }

    DecodeResult *result = core_decode(input);
    if (result->error_code != ERROR_OK || !result->output) {
        return result;
    }

    char *current = g_strdup(result->output);
    int depth = 1;
    GString *format_chain = g_string_new(result->format);

    while (depth < max_depth) {
        DecodeResult *next = core_decode(current);
        if (next->error_code != ERROR_OK ||
            strcmp(next->format, "Unknown") == 0 ||
            strcmp(next->format, "Cached") == 0 ||
            strlen(next->output) == strlen(current)) {
            decode_result_free(next);
            break;
        }

        g_free(result->output);
        result->output = g_strdup(next->output);
        g_string_append_printf(format_chain, " -> %s", next->format);
        result->processing_time_ms += next->processing_time_ms;
        result->decode_depth = depth;

        g_free(current);
        current = g_strdup(next->output);
        decode_result_free(next);
        depth++;
    }

    g_free(result->format);
    result->format = g_string_free(format_chain, FALSE);
    g_free(current);

    return result;
}

EncodeResult* core_encode(const char *input, CoreMode mode) {
    if (!input) {
        return NULL;
    }

    EncodeResult *result = g_new0(EncodeResult, 1);
    result->input_size = strlen(input);

    switch (mode) {
        case CORE_MODE_MORSE:
            result->output = encode_morse(input);
            result->format = g_strdup("Morse");
            break;
        case CORE_MODE_BASE64:
            result->output = encode_base64(input);
            result->format = g_strdup("Base64");
            break;
        case CORE_MODE_HEX:
            result->output = encode_hex(input);
            result->format = g_strdup("Hex");
            break;
        case CORE_MODE_BINARY:
            result->output = encode_binary(input);
            result->format = g_strdup("Binary");
            break;
        default:
            result->output = encode_base64(input);
            result->format = g_strdup("Base64");
            break;
    }

    if (result->output) {
        result->error_code = ERROR_OK;
        result->output_size = strlen(result->output);
    } else {
        result->error_code = ERROR_ENCODE_FAILED;
    }

    return result;
}

void core_cache_clear(void) {
    if (g_core && g_core->cache) {
        g_mutex_lock(&g_core->cache_mutex);
        lru_cache_clear(g_core->cache);
        g_mutex_unlock(&g_core->cache_mutex);
        log_info("Cache cleared");
    }
}

void core_cache_set_max_size(size_t size) {
    if (g_core) {
        g_mutex_lock(&g_core->cache_mutex);
        LRUCache *new_cache = lru_cache_new(size);
        lru_cache_free(g_core->cache);
        g_core->cache = new_cache;
        g_mutex_unlock(&g_core->cache_mutex);
        log_info("Cache size set to %zu", size);
    }
}

CoreStats core_get_stats(void) {
    CoreStats stats = {0};
    if (g_core) {
        g_mutex_lock(&g_core->stats_mutex);
        stats = g_core->stats;
        stats.active_plugins = g_list_length(plugin_manager_list(g_core->plugin_manager));
        g_mutex_unlock(&g_core->stats_mutex);
    }
    return stats;
}

void core_reset_stats(void) {
    if (g_core) {
        g_mutex_lock(&g_core->stats_mutex);
        memset(&g_core->stats, 0, sizeof(CoreStats));
        g_mutex_unlock(&g_core->stats_mutex);
        log_info("Statistics reset");
    }
}

void decode_result_free(DecodeResult *result) {
    if (result) {
        g_free(result->output);
        g_free(result->format);
        g_free(result->input);
        g_free(result->error_message);
        g_free(result);
    }
}

void encode_result_free(EncodeResult *result) {
    if (result) {
        g_free(result->output);
        g_free(result->format);
        g_free(result);
    }
}

PluginManager* core_get_plugin_manager(void) {
    return g_core ? g_core->plugin_manager : NULL;
}

DecodeResult* core_decode_with_mode(const char *input, CoreMode mode) {
    if (!core_init()) {
        DecodeResult *result = g_new0(DecodeResult, 1);
        result->error_code = ERROR_GENERIC;
        result->error_message = g_strdup("Core not initialized");
        return result;
    }

    if (!input || strlen(input) == 0) {
        DecodeResult *result = g_new0(DecodeResult, 1);
        result->error_code = ERROR_INVALID_INPUT;
        result->error_message = g_strdup("Empty input");
        return result;
    }

    DecodeResult *result = g_new0(DecodeResult, 1);
    result->input = g_strdup(input);
    result->mode = mode;

    if (mode == CORE_MODE_AUTO) {
        DecodeResult *auto_res = core_decode(input);
        if (auto_res) {
            result->output = g_strdup(auto_res->output);
            result->format = g_strdup(auto_res->format);
            result->error_code = auto_res->error_code;
            result->error_message = g_strdup(auto_res->error_message);
            decode_result_free(auto_res);
        } else {
            result->error_code = ERROR_DECODE_FAILED;
            result->error_message = g_strdup("Auto decode failed");
        }
        return result;
    }

    switch (mode) {
        case CORE_MODE_MORSE:
            result->output = decode_morse(input);
            result->format = g_strdup("Morse");
            break;
        case CORE_MODE_BASE64:
            result->output = decode_base64(input);
            result->format = g_strdup("Base64");
            break;
        case CORE_MODE_HEX:
            result->output = decode_hex(input);
            result->format = g_strdup("Hex");
            break;
        case CORE_MODE_BINARY:
            result->output = decode_binary(input);
            result->format = g_strdup("Binary");
            break;
        default:
            result->error_code = ERROR_UNKNOWN_FORMAT;
            result->error_message = g_strdup("Unknown mode");
            result->output = g_strdup("");
            result->format = g_strdup("Unknown");
            break;
    }

    if (result->output && !result->error_code) {
        result->error_code = ERROR_OK;
        result->output_size = strlen(result->output);
    }

    return result;
}
