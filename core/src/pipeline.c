#include "../include/plugin.h"
#include "../include/pipeline.h"
#include "../include/score.h"
#include "../include/core.h"
#include "../include/decoder.h"
#include <glib.h>
#include <ctype.h>
#include <string.h>

// Global thread pool for pipeline execution (reused across all searches)
static GThreadPool *g_pipeline_pool = NULL;
static GMutex g_pool_mutex;
static gboolean g_pool_initialized = FALSE;

// Forward declaration
static void pipeline_executor_worker(gpointer data, gpointer user_data);

static void init_pipeline_pool(void) {
    g_mutex_lock(&g_pool_mutex);
    if (!g_pool_initialized) {
        GError *error = NULL;
        g_pipeline_pool = g_thread_pool_new(pipeline_executor_worker, NULL, 4, FALSE, &error); // Use 4 threads for parallelism
        if (error) {
            g_error_free(error);
            g_pipeline_pool = NULL;
        }
        g_pool_initialized = TRUE;
    }
    g_mutex_unlock(&g_pool_mutex);
}

// Fast hash function for deduplication (replaces slow hex string generation)
// buffer_hash_fast is now provided by buffer.c/buffer.h

// Deduplication keys now use buffer_hash_fast (32-bit) to save RAM
// instead of generating massive hex strings.

static int compare_candidate(const void *a, const void *b) {
    const Candidate *ca = (const Candidate*)a;
    const Candidate *cb = (const Candidate*)b;
    if (cb->score > ca->score) return 1;
    if (cb->score < ca->score) return -1;
    return 0;
}

static StepInfo* step_info_new(const char *name, Buffer buf) {
    StepInfo *si = g_new0(StepInfo, 1);
    si->name = g_strdup(name);
    si->buf = buffer_clone(&buf);
    return si;
}

static void step_info_free(StepInfo *si) {
    if (si) {
        g_free(si->name);
        buffer_free(&si->buf);
        g_free(si);
    }
}

static StepInfo* step_info_clone(const StepInfo *src) {
    return step_info_new(src->name, src->buf);
}

void candidate_free(Candidate *c) {
    if (c) {
        buffer_free(&c->buf);
        g_list_free_full(c->history, (GDestroyNotify)step_info_free);
        g_list_free_full(c->steps, g_free);
        g_free(c->meta);
        g_free(c);
    }
}

static Candidate* candidate_clone(const Candidate *src) {
    Candidate *dst = g_new0(Candidate, 1);
    dst->buf = buffer_clone(&src->buf);
    dst->score = src->score;
    dst->history = g_list_copy_deep(src->history, (GCopyFunc)step_info_clone, NULL);
    dst->steps = g_list_copy_deep(src->steps, (GCopyFunc)g_strdup, NULL);
    dst->meta = g_strdup(src->meta);
    return dst;
}

typedef struct {
    Plugin *plugin;
    int priority;
} PlannedPlugin;

typedef struct {
    int max_depth;
    int beam_width;
    int retry_count;
    GList *plugins; // PlannedPlugin*
} PipelinePlan;

// Cache format detection results in candidate
typedef struct {
    gboolean checked;
    gboolean is_base64;
    gboolean is_hex;
    gboolean is_binary;
    gboolean is_morse;
    gboolean is_url;
} FormatCache;

static double candidate_pipeline_score(const Candidate *cand, double parent_readability);
static double step_confidence_boost(const char *step_name);
static gboolean should_stop_on_readable_text(const Candidate *cand);
static gboolean is_high_confidence_result(const Candidate *cand);
static gboolean is_exploratory_transform(const char *step_name);
static gboolean should_skip_plugin_for_candidate(const Candidate *cand, const Plugin *plugin, int depth);
static const char* candidate_last_step(const Candidate *cand);
static gboolean has_structured_artifact(const Buffer *buf);
static gboolean is_structured_transform(const char *step_name);
static double printable_ratio(const Buffer *buf);
static gboolean is_promising_text_candidate(const Buffer *buf);
static int adaptive_beam_width(GList *candidates, int base_width);

static gboolean is_base64_like(const Buffer *buf) {
    if (!buf || !buf->data || buf->len < 4) return FALSE;
    size_t significant = 0;
    for (size_t i = 0; i < buf->len; i++) {
        unsigned char c = buf->data[i];
        if (g_ascii_isspace(c)) continue;
        significant++;
        if (!(g_ascii_isalnum(c) || c == '+' || c == '/' || c == '=' || c == '-' || c == '_')) {
            return FALSE;
        }
    }
    return significant >= 4;
}

static gboolean is_hex_like(const Buffer *buf) {
    if (!buf || !buf->data || buf->len < 2) return FALSE;
    size_t significant = 0;
    for (size_t i = 0; i < buf->len; i++) {
        unsigned char c = buf->data[i];
        if (g_ascii_isspace(c)) continue;
        significant++;
        if (!g_ascii_isxdigit(c)) return FALSE;
    }
    return significant >= 4 && (significant % 2 == 0);
}

static gboolean is_binary_like(const Buffer *buf) {
    if (!buf || !buf->data || buf->len < 4) return FALSE;
    size_t bits = 0;
    for (size_t i = 0; i < buf->len; i++) {
        unsigned char c = buf->data[i];
        if (g_ascii_isspace(c)) continue;
        if (c != '0' && c != '1') return FALSE;
        bits++;
    }
    return bits >= 8;
}

static gboolean is_morse_like(const Buffer *buf) {
    if (!buf || !buf->data || buf->len < 3) return FALSE;
    gboolean has_symbol = FALSE;
    for (size_t i = 0; i < buf->len; i++) {
        unsigned char c = buf->data[i];
        if (c == '.' || c == '-' || c == '/' || g_ascii_isspace(c)) {
            if (c == '.' || c == '-') has_symbol = TRUE;
            continue;
        }
        return FALSE;
    }
    return has_symbol;
}

static gboolean is_url_like(const Buffer *buf) {
    if (!buf || !buf->data || buf->len < 3) return FALSE;
    const char *text = (const char*)buf->data;
    return g_strstr_len(text, buf->len, "%") != NULL ||
           g_strstr_len(text, buf->len, "http") != NULL ||
           g_strstr_len(text, buf->len, "+") != NULL;
}

static gboolean is_alpha_text(const Buffer *buf) {
    if (!buf || !buf->data || buf->len == 0) return FALSE;
    size_t alpha = 0;
    size_t printable = 0;
    for (size_t i = 0; i < buf->len; i++) {
        unsigned char c = buf->data[i];
        if (isprint(c)) printable++;
        if (isalpha(c)) alpha++;
    }
    return printable > 0 && alpha >= printable / 2;
}

static double printable_ratio(const Buffer *buf) {
    if (!buf || !buf->data || buf->len == 0) return 0.0;
    size_t printable = 0;
    for (size_t i = 0; i < buf->len; i++) {
        if (g_ascii_isprint(buf->data[i])) printable++;
    }
    return (double)printable / (double)buf->len;
}

__attribute__((unused))
static gboolean is_promising_text_candidate(const Buffer *buf) {
    if (!buf || !buf->data || buf->len == 0) return FALSE;
    if (has_structured_artifact(buf)) return FALSE;
    if (printable_ratio(buf) < 0.92) return FALSE;
    if (!is_alpha_text(buf)) return FALSE;
    return score_readability(buf->data, buf->len) >= 0.75;
}

static int plugin_priority_for_input(Plugin *p, const Buffer *input) {
    int priority = p->priority * 10;
    if (p->detect && p->detect(*input)) priority += 120;

    // Check if input is pure binary (0, 1, and space only)
    gboolean is_pure_binary = is_binary_like(input);
    gboolean is_pure_hex = is_hex_like(input);
    
    if (g_strcmp0(p->name, "Binary") == 0 && is_pure_binary) priority += 150; // Massively boost Binary
    if (g_strcmp0(p->name, "Hex") == 0 && is_pure_hex) priority += 100;
    if (g_strcmp0(p->name, "Base64") == 0 && is_base64_like(input) && !is_pure_hex && !is_pure_binary) priority += 70;
    
    if (g_strcmp0(p->name, "Morse") == 0 && is_morse_like(input)) priority += 70;
    if (g_strcmp0(p->name, "URL") == 0 && is_url_like(input)) priority += 60;
    if ((g_strcmp0(p->name, "ROT13") == 0 ||
         g_strcmp0(p->name, "Caesar") == 0 ||
         g_strcmp0(p->name, "Atbash") == 0) && is_alpha_text(input)) {
        priority += 35;
    }
    if (g_strcmp0(p->name, "XOR") == 0) priority += 20;
    if (g_strcmp0(p->name, "Scramble") == 0) priority += 10;

    return priority;
}

static int compare_planned_plugin(const void *a, const void *b) {
    const PlannedPlugin *pa = (const PlannedPlugin*)a;
    const PlannedPlugin *pb = (const PlannedPlugin*)b;
    return pb->priority - pa->priority;
}

static void planned_plugin_free(PlannedPlugin *pp) {
    g_free(pp);
}

static GList* build_planned_plugins(PluginManager *pm, const Buffer *input) {
    GList *plugins = plugin_manager_list(pm);
    GList *planned = NULL;

    for (GList *iter = plugins; iter; iter = iter->next) {
        Plugin *p = (Plugin*)iter->data;
        if (!p->enabled || (!p->decode_single && !p->decode_multi)) continue;

        PlannedPlugin *pp = g_new0(PlannedPlugin, 1);
        pp->plugin = p;
        pp->priority = plugin_priority_for_input(p, input);
        planned = g_list_append(planned, pp);
    }

    g_list_free(plugins);
    return g_list_sort(planned, (GCompareFunc)compare_planned_plugin);
}

static void pipeline_plan_free(PipelinePlan *plan) {
    if (!plan) return;
    g_list_free_full(plan->plugins, (GDestroyNotify)planned_plugin_free);
    g_free(plan);
}

static PipelinePlan* build_pipeline_plan(const Buffer *input, int max_depth, int beam_width) {
    PluginManager *pm = core_get_plugin_manager();
    if (!pm) return NULL;

    PipelinePlan *plan = g_new0(PipelinePlan, 1);
    plan->max_depth = max_depth;
    plan->beam_width = beam_width;
    plan->retry_count = 2;
    plan->plugins = build_planned_plugins(pm, input);

    if (is_base64_like(input) || is_hex_like(input) || is_binary_like(input) || is_morse_like(input)) {
        plan->max_depth = MIN(max_depth + 2, 6);
        plan->beam_width += 2;
    } else if (is_alpha_text(input)) {
        plan->max_depth = MIN(max_depth + 1, 5);
    } else {
        plan->retry_count = 3;
    }

    return plan;
}

static double candidate_pipeline_score(const Candidate *cand, double parent_readability) {
    int step_count = g_list_length(cand->steps);
    double score = score_readability(cand->buf.data, cand->buf.len);
    
    // Depth penalty (increased per user suggestion)
    score -= 0.12 * step_count;
    
    // Bonus for readable text
    if (should_stop_on_readable_text(cand)) {
        if (!has_structured_artifact(&cand->buf)) {
            score += 0.6;   // Real text
        } else {
            score += 0.1;   // Encoded text
        }
    }
    
    // Feature: apply structural confidence boost compound across entire chain
    // (with anti-exploit momentum decay to prevent rewarding infinite garbage decodes)
    double momentum = 0.0;
    for (GList *iter = cand->steps; iter; iter = iter->next) {
        momentum = (momentum * 0.7) + step_confidence_boost((const char*)iter->data);
    }
    
    // The infinite loop problem is mathematically clamped by the multiplicative decay (x0.7) 
    // overtaking the linear depth penalty (-0.12).
    score += momentum;
    
    // Evaluate readability collapse
    const char *last = candidate_last_step(cand);
    double improvement = score_readability(cand->buf.data, cand->buf.len) - parent_readability;
    
    if (improvement < -0.15 && is_exploratory_transform(last)) {
        score -= 0.4; // Only punish exploratory mutations that collapse readability into garbage
    }
    const char *prev = (step_count > 1) ? 
        g_list_nth_data(cand->steps, step_count - 2) : NULL;
    
    if (prev && last) {
        // Base64 -> Hex is common pattern
        if (g_strcmp0(prev, "Base64") == 0 && g_strcmp0(last, "Hex") == 0) {
            score += 0.1;
        }
        // Penalize unlikely chains (repeated transforms)
        if (g_strcmp0(prev, last) == 0) {
            if (is_exploratory_transform(last)) {
                score -= 1.0; // Very harsh penalty for looping XOR / ROT13
            } else {
                score -= 0.6; // Increased friction for repeated structured decodes (Base64 -> Base64)
            }
        }
        // Hex -> Base64 is also common
        if (g_strcmp0(prev, "Hex") == 0 && g_strcmp0(last, "Base64") == 0) {
            score += 0.6;
        }
        if (g_strcmp0(prev, "Base64") == 0 && g_strcmp0(last, "Morse") == 0) {
            score += 0.4;
        }
    }
    
    return score;
}

static double step_confidence_boost(const char *step_name) {
    if (!step_name) return 0.0;

    if (g_strcmp0(step_name, "Binary") == 0) return 0.38;
    if (g_strcmp0(step_name, "Hex") == 0) return 0.32;
    if (g_strcmp0(step_name, "Base64") == 0) return 0.28;
    if (g_strcmp0(step_name, "Morse") == 0) return 0.24;
    if (g_strcmp0(step_name, "URL") == 0) return 0.14;
    if (g_strcmp0(step_name, "ROT13") == 0) return 0.06;
    if (g_strcmp0(step_name, "Caesar") == 0) return 0.04;
    if (g_strcmp0(step_name, "Atbash") == 0) return 0.04;
    if (g_strcmp0(step_name, "XOR") == 0) return -0.10;
    if (g_strcmp0(step_name, "Scramble") == 0) return -0.18;
    return 0.0;
}

static gboolean is_exploratory_transform(const char *step_name) {
    if (!step_name) return FALSE;
    return g_strcmp0(step_name, "ROT13") == 0 ||
           g_strcmp0(step_name, "Caesar") == 0 ||
           g_strcmp0(step_name, "Atbash") == 0 ||
           g_strcmp0(step_name, "XOR") == 0 ||
           g_strcmp0(step_name, "Scramble") == 0;
}

__attribute__((unused))
static gboolean is_structured_transform(const char *step_name) {
    if (!step_name) return FALSE;
    return g_strcmp0(step_name, "Binary") == 0 ||
           g_strcmp0(step_name, "Hex") == 0 ||
           g_strcmp0(step_name, "Base64") == 0 ||
           g_strcmp0(step_name, "Morse") == 0 ||
           g_strcmp0(step_name, "URL") == 0;
}

static gboolean has_structured_artifact(const Buffer *buf) {
    if (!buf || !buf->data || buf->len == 0) return FALSE;
    return is_binary_like(buf) || is_hex_like(buf) || is_base64_like(buf) || is_morse_like(buf) || is_url_like(buf);
}

// Early termination: check if candidate is high-confidence result
static gboolean is_high_confidence_result(const Candidate *cand) {
    if (!cand || !cand->buf.data || cand->buf.len == 0) return FALSE;
    double score = score_readability(cand->buf.data, cand->buf.len);
    return score > 1.8 && 
           !has_structured_artifact(&cand->buf) &&
           is_alpha_text(&cand->buf);
}

static gboolean should_stop_on_readable_text(const Candidate *cand) {
    if (!cand || !cand->buf.data || cand->buf.len == 0) return FALSE;
    if (has_structured_artifact(&cand->buf)) return FALSE;
    if (!is_alpha_text(&cand->buf)) return FALSE;

    size_t printable = 0;
    size_t alpha = 0;
    size_t vowels = 0;
    size_t spaces = 0;

    for (size_t i = 0; i < cand->buf.len; i++) {
        unsigned char c = cand->buf.data[i];
        if (g_ascii_isprint(c)) printable++;
        if (g_ascii_isalpha(c)) {
            alpha++;
            switch (g_ascii_tolower(c)) {
                case 'a':
                case 'e':
                case 'i':
                case 'o':
                case 'u':
                    vowels++;
                    break;
                default:
                    break;
            }
        } else if (g_ascii_isspace(c)) {
            spaces++;
        }
    }

    if (printable == 0) return FALSE;
    if (alpha >= MAX((size_t)3, printable * 3 / 5) &&
        (vowels > 0 || spaces > 0) &&
        printable * 10 >= cand->buf.len * 9) {
        return TRUE;
    }

    return score_readability(cand->buf.data, cand->buf.len) >= 1.05;
}

static gboolean should_skip_plugin_for_candidate(const Candidate *cand, const Plugin *plugin, int depth __attribute__((unused))) {
    if (!cand || !plugin) return TRUE;


    
    // Removed hard block for sequential identical plugins (e.g., Hex -> Hex)
    // The robust candidate_pipeline_score and visited deduplication will correctly
    // prune infinite loops while allowing valid nested encodings.
    // We ALSO do not return TRUE on readable_text, because the text might just be
    // intermediate Base64 output that coincidentally passes the loose english heuristic!
    
    // Skip exploratory transforms on structured data
    if (has_structured_artifact(&cand->buf) && is_exploratory_transform(plugin->name)) {
        return TRUE;
    }
    
    return FALSE;
}

static GList* create_input_variants(const Buffer *input) {
    GList *variants = NULL;

    Buffer *original = g_new0(Buffer, 1);
    *original = buffer_clone(input);
    variants = g_list_append(variants, original);

    GString *compact = g_string_sized_new(input->len);
    for (size_t i = 0; i < input->len; i++) {
        unsigned char c = input->data[i];
        if (!g_ascii_isspace(c)) g_string_append_c(compact, (char)c);
    }
    if (compact->len > 0 && compact->len != input->len) {
        Buffer *trimmed = g_new0(Buffer, 1);
        *trimmed = buffer_new((const unsigned char*)compact->str, compact->len);
        variants = g_list_append(variants, trimmed);
    }
    g_string_free(compact, TRUE);

    if (is_hex_like(input)) {
        GString *lower = g_string_sized_new(input->len);
        for (size_t i = 0; i < input->len; i++) {
            unsigned char c = input->data[i];
            if (!g_ascii_isspace(c)) g_string_append_c(lower, (char)g_ascii_tolower(c));
        }
        if (lower->len > 0) {
            Buffer *hex_lower = g_new0(Buffer, 1);
            *hex_lower = buffer_new((const unsigned char*)lower->str, lower->len);
            variants = g_list_append(variants, hex_lower);
        }
        g_string_free(lower, TRUE);
    }

    return variants;
}

static void buffer_box_free(Buffer *buf) {
    if (!buf) return;
    buffer_free(buf);
    g_free(buf);
}

static const char* candidate_last_step(const Candidate *cand) {
    GList *last = g_list_last(cand->steps);
    return last ? (const char*)last->data : NULL;
}

static Candidate* build_decoded_candidate(const Candidate *cand, const char *step_name, char *decoded) {
    if (!decoded || !decoded[0]) {
        g_free(decoded);
        return NULL;
    }

    Buffer next_buf = buffer_new((const unsigned char*)decoded, strlen(decoded));
    g_free(decoded);
    if (!next_buf.data || buffer_equal(&cand->buf, &next_buf)) {
        buffer_free(&next_buf);
        return NULL;
    }

    Candidate *next = g_new0(Candidate, 1);
    next->buf = next_buf;
    next->history = g_list_copy_deep(cand->history, (GCopyFunc)step_info_clone, NULL);
    next->history = g_list_append(next->history, step_info_new(step_name, next_buf));
    next->steps = g_list_copy_deep(cand->steps, (GCopyFunc)g_strdup, NULL);
    next->steps = g_list_append(next->steps, g_strdup(step_name));
    next->meta = g_strdup("");
    next->score = candidate_pipeline_score(next, score_readability(cand->buf.data, cand->buf.len));
    return next;
}

static GList* generate_builtin_candidates(const Candidate *cand) {
    GList *produced = NULL;
    char *input = g_strndup((const char*)cand->buf.data, cand->buf.len);
    if (!input) return NULL;

    // We no longer hard-stop on readable text here, as intermediate 
    // structured formats (like Base64) can sometimes pass as text.

    gboolean exact_binary = is_binary(input, cand->buf.len);
    gboolean exact_hex = is_hex(input, cand->buf.len);
    gboolean exact_morse = is_morse(input, cand->buf.len);
    gboolean exact_base64 = is_base64(input, cand->buf.len);

    if (exact_binary) {
        Candidate *next = build_decoded_candidate(cand, "Binary", decode_binary(input));
        if (next) produced = g_list_append(produced, next);
    }

    if (exact_hex) {
        Candidate *next = build_decoded_candidate(cand, "Hex", decode_hex(input));
        if (next) produced = g_list_append(produced, next);
    }

    if (exact_morse) {
        Candidate *next = build_decoded_candidate(cand, "Morse", decode_morse(input));
        if (next) produced = g_list_append(produced, next);
    }

    if (exact_base64) {
        Candidate *next = build_decoded_candidate(cand, "Base64", decode_base64(input));
        if (next) produced = g_list_append(produced, next);
    }

    g_free(input);
    return produced;
}

typedef struct {
    const Candidate *cand;
    Plugin *plugin;
    GMutex *results_mutex;
    GCond *results_cond;
    GList **results;
    volatile gint *pending_tasks;
} PipelineTask;

static void pipeline_task_free(PipelineTask *task) {
    g_free(task);
}

static void pipeline_executor_worker(gpointer data, gpointer user_data) {
    (void)user_data;
    // Removed artificial sleep delay for massive performance boost
    PipelineTask *task = (PipelineTask*)data;
    const Candidate *cand = task->cand;
    Plugin *p = task->plugin;
    GList *results = NULL;
    GList *produced = NULL;

    if (p->decode_multi) {
        results = p->decode_multi(cand->buf);
    } else if (p->decode_single) {
        Buffer out = p->decode_single(cand->buf);
        if (out.data) {
            Buffer *boxed = g_new0(Buffer, 1);
            *boxed = out;
            results = g_list_append(NULL, boxed);
        }
    }

    for (GList *r = results; r; r = r->next) {
        Buffer *res = (Buffer*)r->data;
        if (!res->data || res->len == 0) {
            buffer_box_free(res);
            continue;
        }
        if (buffer_equal(&cand->buf, res)) {
            buffer_box_free(res);
            continue;
        }

        Candidate *next = g_new0(Candidate, 1);
        next->buf = *res;
        g_free(res);
        next->history = g_list_copy_deep(cand->history, (GCopyFunc)step_info_clone, NULL);
        next->history = g_list_append(next->history, step_info_new(p->name, next->buf));
        next->steps = g_list_copy_deep(cand->steps, (GCopyFunc)g_strdup, NULL);
        next->steps = g_list_append(next->steps, g_strdup(p->name));
        next->meta = g_strdup(p->meta_info ? p->meta_info : "");
        double raw_score = score_readability(next->buf.data, next->buf.len);
        next->score = candidate_pipeline_score(next, raw_score);

        // Early pruning: don't even add to list if it's pure garbage
        // Threshold is intentionally very low to avoid missing nested encoding steps
        if (next->score < 0.04 && next->buf.len > 12) {
            candidate_free(next);
            continue;
        }
        
        produced = g_list_append(produced, next);
    }
    g_list_free(results);

    if (produced) {
        g_mutex_lock(task->results_mutex);
        *task->results = g_list_concat(*task->results, produced);
        g_mutex_unlock(task->results_mutex);
    }

    volatile gint *pending = task->pending_tasks;
    GCond *cond = task->results_cond;
    
    pipeline_task_free(task);
    
    if (pending && cond) {
        if (g_atomic_int_dec_and_test(pending)) {
            g_cond_signal(cond);
        }
    }
}

static GList* dedupe_candidate_results(GList *candidates, GHashTable *visited) {
    GList *deduped = NULL;
    for (GList *iter = candidates; iter; iter = iter->next) {
        Candidate *cand = (Candidate*)iter->data;
        guint64 hash = buffer_hash_fast(&cand->buf);
        
        gint64 *hash_key = g_new(gint64, 1);
        *hash_key = hash;
        
        if (g_hash_table_contains(visited, hash_key)) {
            g_free(hash_key);
            candidate_free(cand);
            continue;
        }
        g_hash_table_add(visited, hash_key);
        deduped = g_list_prepend(deduped, cand);
    }
    g_list_free(candidates);
    return g_list_reverse(deduped);
}

// Adaptive beam width based on score variance
// Intelligent beam width management based on score distribution and variance
static int adaptive_beam_width(GList *candidates, int base_width) {
    if (!candidates) return base_width;
    size_t count = g_list_length(candidates);
    if (count < 2) return base_width;

    // Compute score stats over top-N candidates to decide beam width.
    size_t sample_n = MIN(count, (size_t)MAX(base_width, 10));
    double mean = 0.0;
    size_t i = 0;

    // First pass: mean
    for (GList *iter = candidates; iter && i < sample_n; iter = iter->next, i++) {
        Candidate *cand = (Candidate*)iter->data;
        mean += cand->score;
    }
    mean /= (double)sample_n;

    // Second pass: variance
    double var = 0.0;
    i = 0;
    for (GList *iter = candidates; iter && i < sample_n; iter = iter->next, i++) {
        Candidate *cand = (Candidate*)iter->data;
        double d = cand->score - mean;
        var += d * d;
    }
    var /= (double)sample_n;
    double stddev = sqrt(var);

    Candidate *top = (Candidate*)candidates->data;

    // Heuristic rules:
    // - very confident (high top score, low spread) => narrow beam
    // - uncertain (high spread) => widen beam for diversity
    if (top->score > 1.2 && stddev < 0.25) return MAX(base_width / 2, 5);
    if (top->score > 0.9 && stddev < 0.35) return MAX(base_width - 2, 8);

    // If distribution is tight => moderate shrink
    if (stddev < 0.18) return MAX(base_width - 1, 6);

    // If distribution is wide => widen significantly
    if (stddev > 0.55) return MIN(base_width * 2, 50);
    if (stddev > 0.35) return MIN(base_width + 6, 45);

    return base_width;
}


// Map last_step string to a canonical "transform family" for diversity control.
// This prevents beam from being dominated by one family (e.g., many Caesar variants).
static const char* transform_family_from_step(const char *step_name) {
    if (!step_name) return "unknown";
    if (g_strcmp0(step_name, "Base64") == 0) return "Base64";
    if (g_strcmp0(step_name, "Hex") == 0) return "Hex";
    if (g_strcmp0(step_name, "Binary") == 0) return "Binary";
    if (g_strcmp0(step_name, "Morse") == 0) return "Morse";
    if (g_strcmp0(step_name, "URL") == 0) return "URL";

    // Exploratory / letter-mutation family
    if (g_strcmp0(step_name, "ROT13") == 0 ||
        g_strcmp0(step_name, "Caesar") == 0 ||
        g_strcmp0(step_name, "Atbash") == 0) {
        return "CaesarRotAtbash";
    }

    if (g_strcmp0(step_name, "XOR") == 0) return "XOR";
    if (g_strcmp0(step_name, "Scramble") == 0) return "Scramble";

    // Fallback: use raw step_name, but clamp to avoid metadata suffix mismatch.
    return step_name;
}

// Pruning strategy that maintains diversity of transform families in the beam
static GList* prune_and_diversify_beam(GList *candidates, int target_width) {
    if (!candidates) return NULL;
    if ((int)g_list_length(candidates) <= target_width) return candidates;

    GList *result = NULL;
    GHashTable *type_counts = g_hash_table_new(g_str_hash, g_str_equal);
    int total_added = 0;

    // Max candidates per canonical family.
    // Slightly higher than before to avoid losing nested structured encodings.
    int max_per_type = MAX(2, target_width / 3);

    GList *next;
    for (GList *iter = candidates; iter && total_added < target_width; iter = next) {
        next = iter->next;
        Candidate *cand = (Candidate*)iter->data;

        const char *last_step = candidate_last_step(cand);
        const char *family = transform_family_from_step(last_step);
        if (!family) family = "unknown";

        int count = GPOINTER_TO_INT(g_hash_table_lookup(type_counts, family));

        // Keep candidate if it's high quality or if its family isn't over-represented.
        // Also allow 1 extra slot for top candidates.
        int family_capacity = max_per_type;
        if (cand->score > 1.2) family_capacity = max_per_type + 1;

        if (cand->score > 1.0 || count < family_capacity) {
            g_hash_table_insert(type_counts, (gpointer)family, GINT_TO_POINTER(count + 1));

            // Move node from candidates to result
            candidates = g_list_remove_link(candidates, iter);
            iter->next = NULL;
            iter->prev = NULL;
            result = g_list_concat(result, iter);
            total_added++;
        }
    }

    // Clean up remaining candidates
    g_list_free_full(candidates, (GDestroyNotify)candidate_free);
    g_hash_table_destroy(type_counts);

    return result;
}


static GList* execute_planned_pipeline(const Buffer *input, const PipelinePlan *plan) {
    if (!input || !input->data || input->len == 0 || !plan) return NULL;

    // Initialize global thread pool once
    init_pipeline_pool();

    GHashTable *visited = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    GList *beam = NULL;
    GList *all_seen = NULL;

    Candidate *init = g_new0(Candidate, 1);
    init->buf = buffer_clone(input);
    init->score = score_readability(init->buf.data, init->buf.len);
    init->history = g_list_append(NULL, step_info_new("Input", init->buf));
    init->steps = g_list_append(NULL, g_strdup("Input"));
    init->meta = g_strdup("");
    beam = g_list_append(beam, init);
    all_seen = g_list_append(all_seen, candidate_clone(init));
    
    gint64 *init_hash = g_new(gint64, 1);
    *init_hash = buffer_hash_fast(&init->buf);
    g_hash_table_add(visited, init_hash);

    int current_beam_width = plan->beam_width;

    for (int depth = 0; depth < plan->max_depth; depth++) {
        GList *new_beam = NULL;
        GList *frozen_beam = NULL;
        GMutex results_mutex;
        GCond results_cond;
        volatile gint pending_tasks = 0;
        
        g_mutex_init(&results_mutex);
        g_cond_init(&results_cond);
        
        // Use global thread pool instead of creating new one
        GThreadPool *pool = g_pipeline_pool;
        if (pool) {
            for (GList *iter = beam; iter; iter = iter->next) {
                Candidate *cand = (Candidate*)iter->data;
                if (should_stop_on_readable_text(cand)) {
                    Candidate *frozen = candidate_clone(cand);
                    g_mutex_lock(&results_mutex);
                    frozen_beam = g_list_append(frozen_beam, frozen);
                    g_mutex_unlock(&results_mutex);
                    continue;
                }

                GList *builtin = generate_builtin_candidates(cand);
                if (builtin) {
                    g_mutex_lock(&results_mutex);
                    new_beam = g_list_concat(new_beam, builtin);
                    g_mutex_unlock(&results_mutex);
                }

                for (GList *piter = plan->plugins; piter; piter = piter->next) {
                    PlannedPlugin *pp = (PlannedPlugin*)piter->data;
                    Plugin *p = pp->plugin;
                    if (p->detect && !p->detect(cand->buf) && pp->priority < 40) continue;
                    if (should_skip_plugin_for_candidate(cand, p, depth)) continue;

                    PipelineTask *task = g_new0(PipelineTask, 1);
                    task->cand = cand;
                    task->plugin = p;
                    task->results_mutex = &results_mutex;
                    task->results_cond = &results_cond;
                    task->results = &new_beam;
                    task->pending_tasks = &pending_tasks;
                    g_atomic_int_inc(&pending_tasks);
                    
                    GError *error = NULL;
                    if (!g_thread_pool_push(pool, task, &error)) {
                        g_atomic_int_dec_and_test(&pending_tasks);
                        pipeline_task_free(task);
                        if (error) g_error_free(error);
                    }
                }
            }
            // Wait for all locally dispatched tasks to complete using GCond instead of busy wait
            g_mutex_lock(&results_mutex);
            while (g_atomic_int_get(&pending_tasks) > 0) {
                g_cond_wait(&results_cond, &results_mutex);
            }
            g_mutex_unlock(&results_mutex);
        } else {
            // Fallback to sequential processing if pool unavailable

            for (GList *iter = beam; iter; iter = iter->next) {
                Candidate *cand = (Candidate*)iter->data;
                if (should_stop_on_readable_text(cand)) {
                    Candidate *frozen = candidate_clone(cand);
                    frozen_beam = g_list_append(frozen_beam, frozen);
                    continue;
                }

                GList *builtin = generate_builtin_candidates(cand);
                if (builtin) {
                    new_beam = g_list_concat(new_beam, builtin);
                }

                for (GList *piter = plan->plugins; piter; piter = piter->next) {
                    PlannedPlugin *pp = (PlannedPlugin*)piter->data;
                    Plugin *p = pp->plugin;
                    if (p->detect && !p->detect(cand->buf) && pp->priority < 40) continue;
                    if (should_skip_plugin_for_candidate(cand, p, depth)) continue;

                    GList *results = NULL;
                    if (p->decode_multi) {
                        results = p->decode_multi(cand->buf);
                    } else if (p->decode_single) {
                        Buffer out = p->decode_single(cand->buf);
                        if (out.data) {
                            Buffer *boxed = g_new0(Buffer, 1);
                            *boxed = out;
                            results = g_list_append(NULL, boxed);
                        }
                    }

                    for (GList *r = results; r; r = r->next) {
                        Buffer *res = (Buffer*)r->data;
                        if (!res->data || res->len == 0) {
                            buffer_box_free(res);
                            continue;
                        }
                        if (buffer_equal(&cand->buf, res)) {
                            buffer_box_free(res);
                            continue;
                        }

                        Candidate *next = g_new0(Candidate, 1);
                        next->buf = *res;
                        g_free(res);
                        next->history = g_list_copy_deep(cand->history, (GCopyFunc)step_info_clone, NULL);
                        next->history = g_list_append(next->history, step_info_new(p->name, next->buf));
                        next->steps = g_list_copy_deep(cand->steps, (GCopyFunc)g_strdup, NULL);
                        next->steps = g_list_append(next->steps, g_strdup(p->name));
                        next->meta = g_strdup(p->meta_info ? p->meta_info : "");
                        next->score = candidate_pipeline_score(next, score_readability(next->buf.data, next->buf.len));
                        new_beam = g_list_append(new_beam, next);
                    }
                    g_list_free(results);
                }
            }
        }
        g_mutex_clear(&results_mutex);
        g_cond_clear(&results_cond);

        new_beam = dedupe_candidate_results(new_beam, visited);
        new_beam = g_list_concat(new_beam, frozen_beam);

        if (!new_beam) break;

        new_beam = g_list_sort(new_beam, (GCompareFunc)compare_candidate);
        
        // Adaptive beam width
        current_beam_width = adaptive_beam_width(new_beam, plan->beam_width);
        
        // Apply Diversity Filtering + Pruning
        new_beam = prune_and_diversify_beam(new_beam, current_beam_width);

        // Accumulate winners into all_seen
        for (GList *iter = new_beam; iter; iter = iter->next) {
            all_seen = g_list_append(all_seen, candidate_clone((Candidate*)iter->data));
        }

        // Early termination: stop if we found high-confidence result
        if (new_beam && is_high_confidence_result((Candidate*)new_beam->data)) {
            for (GList *iter = beam; iter; iter = iter->next) {
                candidate_free((Candidate*)iter->data);
            }
            g_list_free(beam);
            beam = new_beam;
            break;
        }

        for (GList *iter = beam; iter; iter = iter->next) {
            candidate_free((Candidate*)iter->data);
        }
        g_list_free(beam);
        beam = new_beam;
    }

    g_hash_table_destroy(visited);
    
    // Clean up remaining beam since we return all_seen
    for (GList *iter = beam; iter; iter = iter->next) {
        candidate_free((Candidate*)iter->data);
    }
    g_list_free(beam);

    all_seen = g_list_sort(all_seen, (GCompareFunc)compare_candidate);
    
    GHashTable *final_visited = g_hash_table_new(g_direct_hash, g_direct_equal);
    all_seen = dedupe_candidate_results(all_seen, final_visited);
    g_hash_table_destroy(final_visited);
    
    while ((int)g_list_length(all_seen) > plan->beam_width) {
        GList *last = g_list_last(all_seen);
        candidate_free((Candidate*)last->data);
        all_seen = g_list_delete_link(all_seen, last);
    }

    return all_seen;
}

GList* pipeline_beam_search(Buffer input, int max_depth, int beam_width) {
    if (!input.data || input.len == 0) return NULL;

    PluginManager *pm = core_get_plugin_manager();
    if (!pm) return NULL;

    GHashTable *visited = g_hash_table_new(g_direct_hash, g_direct_equal);
    GList *beam = NULL;

    // Initial candidate
    Candidate *init = g_new0(Candidate, 1);
    init->buf.data = g_malloc(input.len);
    memcpy(init->buf.data, input.data, input.len);
    init->buf.len = input.len;
    init->score = score_readability(init->buf.data, init->buf.len);
    init->steps = g_list_append(NULL, g_strdup("Input"));
    init->history = g_list_append(NULL, step_info_new("Input", init->buf));
    init->meta = g_strdup("");
    beam = g_list_append(beam, init);

    g_hash_table_add(visited, GUINT_TO_POINTER(buffer_hash_fast(&init->buf)));

    for (int depth = 0; depth < max_depth; depth++) {
        GList *new_beam = NULL;
        GList *plugins = plugin_manager_list(pm);

        for (GList *iter = beam; iter; iter = iter->next) {
            Candidate *cand = iter->data;

            for (GList *piter = plugins; piter; piter = piter->next) {
                Plugin *p = (Plugin*)piter->data;
                if (!p->decode_single && !p->decode_multi) continue;
                if (p->detect && !p->detect(cand->buf)) continue;

                GList *results = NULL;
                if (p->decode_multi) {
                    results = p->decode_multi(cand->buf);
                } else if (p->decode_single) {
                    Buffer out = p->decode_single(cand->buf);
                    if (out.data) {
                        Buffer *buf = g_new0(Buffer, 1);
                        *buf = out; // copy struct
                        results = g_list_append(NULL, buf);
                    }
                }

                for (GList *r = results; r; r = r->next) {
                    Buffer *res = (Buffer*)r->data;
                    if (!res->data || res->len == 0) {
                        buffer_box_free(res);
                        continue;
                    }

                    // Skip if unchanged
                    if (res->len == cand->buf.len &&
                        memcmp(res->data, cand->buf.data, res->len) == 0) {
                        g_free(res->data);
                        g_free(res);
                        continue;
                    }

                    guint32 hash = buffer_hash_fast(res);
                    if (g_hash_table_contains(visited, GUINT_TO_POINTER(hash))) {
                        g_free(res->data);
                        g_free(res);
                        continue;
                    }
                    g_hash_table_add(visited, GUINT_TO_POINTER(hash));

                    Candidate *new_cand = g_new0(Candidate, 1);
                    new_cand->buf = *res; // take ownership
                    g_free(res);
                    new_cand->score = score_readability(new_cand->buf.data, new_cand->buf.len);
                    new_cand->steps = g_list_copy_deep(cand->steps, (GCopyFunc)g_strdup, NULL);
                    char *step_name = g_strdup_printf("%s%s", p->name,
                                                       p->meta_info ? p->meta_info : "");
                    new_cand->steps = g_list_append(new_cand->steps, step_name);
                    new_cand->history = g_list_copy_deep(cand->history, (GCopyFunc)step_info_clone, NULL);
                    new_cand->history = g_list_append(new_cand->history, step_info_new(step_name, new_cand->buf));
                    new_cand->meta = g_strdup(p->meta_info ? p->meta_info : "");

                    new_beam = g_list_append(new_beam, new_cand);
                }
                g_list_free(results);
            }
        }
        g_list_free(plugins);

        if (!new_beam) break;

        new_beam = g_list_sort(new_beam, (GCompareFunc)compare_candidate);
        
        // --- Branch Diversity Pruning ---
        // Ensure no single transform type (e.g. only XOR variants) fills the entire beam.
        GHashTable *type_counts = g_hash_table_new(g_str_hash, g_str_equal);
        GList *diversified = NULL;
        GList *discarded = NULL;
        
        for (GList *iter = new_beam; iter; iter = iter->next) {
            Candidate *cand = (Candidate*)iter->data;
            const char *last_step = cand->steps ? (const char*)g_list_last(cand->steps)->data : "None";
            
            char *type_name = g_strdup(last_step);
            char *space = strchr(type_name, ' ');
            if (space) *space = '\0';
            
            int count = GPOINTER_TO_INT(g_hash_table_lookup(type_counts, type_name));
            if (count < 3 || g_list_length(diversified) < (beam_width / 2)) {
                diversified = g_list_append(diversified, cand);
                g_hash_table_insert(type_counts, type_name, GINT_TO_POINTER(count + 1));
            } else {
                discarded = g_list_append(discarded, cand);
            }
            g_free(type_name);
        }
        
        // Clean up
        g_list_free(new_beam);
        g_hash_table_destroy(type_counts);
        for (GList *iter = discarded; iter; iter = iter->next) candidate_free(iter->data);
        g_list_free(discarded);
        
        new_beam = diversified;

        int count = g_list_length(new_beam);
        while (count > beam_width) {
            GList *last = g_list_last(new_beam);
            candidate_free(last->data);
            new_beam = g_list_delete_link(new_beam, last);
            count--;
        }

        for (GList *iter = beam; iter; iter = iter->next)
            candidate_free(iter->data);
        g_list_free(beam);
        beam = new_beam;
    }

    g_hash_table_destroy(visited);
    return beam;
}

void candidate_list_free(GList *list) {
    for (GList *iter = list; iter; iter = iter->next)
        candidate_free(iter->data);
    g_list_free(list);
}

GList* pipeline_smart_search(Buffer input, int max_depth, int beam_width) {
    if (!input.data || input.len == 0) return NULL;

    PipelinePlan *plan = build_pipeline_plan(&input, max_depth, beam_width);
    if (!plan) return NULL;

    GList *variants = create_input_variants(&input);
    GList *all_candidates = NULL;

    int retries = 0;
    for (GList *iter = variants; iter && retries < plan->retry_count; iter = iter->next, retries++) {
        Buffer *variant = (Buffer*)iter->data;
        GList *beam = execute_planned_pipeline(variant, plan);
        for (GList *b = beam; b; b = b->next) {
            Candidate *cand = candidate_clone((Candidate*)b->data);
            all_candidates = g_list_append(all_candidates, cand);
        }
        candidate_list_free(beam);
    }

    g_list_free_full(variants, (GDestroyNotify)buffer_box_free);
    pipeline_plan_free(plan);

    if (!all_candidates) return NULL;

    all_candidates = g_list_sort(all_candidates, (GCompareFunc)compare_candidate);
    while ((int)g_list_length(all_candidates) > beam_width) {
        GList *last = g_list_last(all_candidates);
        candidate_free((Candidate*)last->data);
        all_candidates = g_list_delete_link(all_candidates, last);
    }
    return all_candidates;
}
