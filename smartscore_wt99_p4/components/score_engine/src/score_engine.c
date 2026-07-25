#include "score_engine.h"

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#define SCORE_TEMPO_MIN 0.60f
#define SCORE_TEMPO_MAX 1.80f
#define SCORE_MISSING_COST 8.0f
#define SCORE_EXTRA_COST 6.0f
#define SCORE_BAD_MATCH_COST 9.0f
#define SCORE_INF_COST 6000.0f
#define SCORE_ALIGN_SCALE 10.0f
#define SCORE_ROUGH_GATE_SCALE 2.0f
#define SCORE_FINE_GATE_SCALE 1.0f
#define SCORE_TIME_GATE_EXTENSION_MS 600.0f
#define SCORE_RESYNC_WINDOW 5
#define SCORE_DP_MAX_CELLS (260U * 260U)
#define SCORE_JSON_INITIAL_BYTES (16U * 1024U)
#define SCORE_JSON_MAX_BYTES (1024U * 1024U)

typedef enum {
    ALIGN_DIAG = 0,
    ALIGN_MISSING = 1,
    ALIGN_EXTRA = 2,
} align_op_t;

typedef struct {
    int target_index;
    int played_index;
    align_op_t op;
} alignment_step_t;

typedef struct {
    float tempo_scale;
    float start_offset_ms;
    float gate_scale;
    bool sequence_only;
} alignment_context_t;

typedef struct {
    int pitch_error;
    float time_error_ms;
    float duration_error_ratio;
    float interval_error;
    float adjusted_start_ms;
    float adjusted_duration_ms;
    float hard_gate_ms;
    float cost;
} match_metrics_t;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} json_builder_t;

static float clamp_float(float value, float minimum, float maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static float max_float(float a, float b)
{
    return a > b ? a : b;
}

static void set_error(char *error, size_t error_length, const char *message)
{
    if (error != NULL && error_length > 0) {
        strlcpy(error, message != NULL ? message : "score_engine_failed",
                error_length);
    }
}

static void *preferred_alloc(size_t bytes, bool zeroed)
{
    void *memory = NULL;
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        memory = zeroed
                     ? heap_caps_calloc(1, bytes,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                     : heap_caps_malloc(bytes,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (memory == NULL) {
        memory = zeroed
                     ? heap_caps_calloc(1, bytes,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                     : heap_caps_malloc(bytes,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return memory;
}

static int compare_performance_notes(const void *left, const void *right)
{
    const performance_note_t *a = left;
    const performance_note_t *b = right;
    if (a->start_ms != b->start_ms) {
        return a->start_ms < b->start_ms ? -1 : 1;
    }
    if (a->midi != b->midi) {
        return a->midi < b->midi ? -1 : 1;
    }
    if (a->channel != b->channel) {
        return a->channel < b->channel ? -1 : 1;
    }
    return 0;
}

static float adjusted_start_ms(const score_note_t *note,
                               const alignment_context_t *context)
{
    return context->start_offset_ms +
           (float)note->start_ms * context->tempo_scale;
}

static float adjusted_duration_ms(const score_note_t *note,
                                  const alignment_context_t *context)
{
    return max_float(1.0f, (float)note->duration_ms * context->tempo_scale);
}

static float hard_time_gate_ms(const score_note_t *note,
                               const alignment_context_t *context)
{
    float gate = 550.0f + 1.05f * (float)note->duration_ms *
                              context->tempo_scale;
    gate = clamp_float(gate, 750.0f, 2200.0f);
    return gate * max_float(0.5f, context->gate_scale);
}

static float match_cost(const score_note_t *target,
                        int target_index,
                        const performance_note_t *played,
                        int played_index,
                        const alignment_context_t *context,
                        match_metrics_t *metrics)
{
    int pitch_error = (int)played->midi - (int)target->midi;
    if (context->sequence_only) {
        float cost = pitch_error == 0 ? 0.0f : SCORE_BAD_MATCH_COST;
        if (metrics != NULL) {
            *metrics = (match_metrics_t) {
                .pitch_error = pitch_error,
                .cost = cost,
            };
        }
        return cost;
    }

    float expected_start = adjusted_start_ms(target, context);
    float expected_duration = adjusted_duration_ms(target, context);
    float time_error = fabsf((float)played->start_ms - expected_start);
    float hard_gate = hard_time_gate_ms(target, context);
    float duration_ratio =
        (float)played->duration_ms / max_float(1.0f, expected_duration);
    float duration_error_ratio =
        clamp_float(fabsf(duration_ratio - 1.0f), 0.0f, 3.0f);
    float interval_error = 0.0f;

    if (target_index > 0 && played_index > 0) {
        int target_interval = (int)target->midi - (int)(target - 1)->midi;
        int played_interval = (int)played->midi - (int)(played - 1)->midi;
        interval_error = fabsf((float)(target_interval - played_interval));
    }

    bool exact_pitch = pitch_error == 0;
    bool outside_gate = time_error > hard_gate;
    if ((!exact_pitch && time_error > hard_gate + SCORE_TIME_GATE_EXTENSION_MS) ||
        (exact_pitch && time_error > hard_gate + 2.0f * SCORE_TIME_GATE_EXTENSION_MS)) {
        if (metrics != NULL) {
            *metrics = (match_metrics_t) {
                .pitch_error = pitch_error,
                .time_error_ms = time_error,
                .duration_error_ratio = duration_error_ratio,
                .interval_error = interval_error,
                .adjusted_start_ms = expected_start,
                .adjusted_duration_ms = expected_duration,
                .hard_gate_ms = hard_gate,
                .cost = SCORE_INF_COST,
            };
        }
        return SCORE_INF_COST;
    }

    /* Exact MIDI pitch is binary. Pitch distance is not softened or corrected. */
    float pitch_component = exact_pitch ? 0.0f : 10.0f;
    float soft_gate = clamp_float(260.0f + 0.45f *
                                  (float)target->duration_ms *
                                  context->tempo_scale,
                                  320.0f, 1100.0f) *
                      max_float(0.5f, context->gate_scale);
    float time_component = clamp_float(time_error / max_float(1.0f, soft_gate) * 6.0f,
                                       0.0f, 10.0f);
    if (outside_gate) {
        time_component += clamp_float((time_error - hard_gate) /
                                          SCORE_TIME_GATE_EXTENSION_MS * 4.0f,
                                      0.0f, 6.0f);
    }
    float duration_component =
        clamp_float(duration_error_ratio * 8.0f, 0.0f, 10.0f);
    float interval_component = clamp_float(interval_error * 1.2f, 0.0f, 10.0f);
    float cost = 0.40f * pitch_component +
                 0.25f * time_component +
                 0.20f * duration_component +
                 0.15f * interval_component;
    if (!exact_pitch) {
        cost = max_float(cost, SCORE_BAD_MATCH_COST);
    }
    cost = clamp_float(cost, 0.0f, SCORE_BAD_MATCH_COST);

    if (metrics != NULL) {
        *metrics = (match_metrics_t) {
            .pitch_error = pitch_error,
            .time_error_ms = time_error,
            .duration_error_ratio = duration_error_ratio,
            .interval_error = interval_error,
            .adjusted_start_ms = expected_start,
            .adjusted_duration_ms = expected_duration,
            .hard_gate_ms = hard_gate,
            .cost = cost,
        };
    }
    return cost;
}

static uint16_t scaled_cost(float cost)
{
    cost = clamp_float(cost, 0.0f, SCORE_INF_COST);
    return (uint16_t)(cost * SCORE_ALIGN_SCALE + 0.5f);
}

static uint16_t add_cost(uint16_t base, uint16_t cost)
{
    uint32_t sum = (uint32_t)base + cost;
    uint16_t maximum = scaled_cost(SCORE_INF_COST);
    return sum > maximum ? maximum : (uint16_t)sum;
}

static bool find_resync(const score_note_t *target,
                        size_t target_count,
                        const performance_note_t *played,
                        size_t played_count,
                        size_t target_index,
                        size_t played_index,
                        size_t *out_target,
                        size_t *out_played)
{
    for (size_t distance = 1; distance <= SCORE_RESYNC_WINDOW; ++distance) {
        for (size_t target_skip = 0; target_skip <= distance; ++target_skip) {
            size_t played_skip = distance - target_skip;
            size_t ti = target_index + target_skip;
            size_t pi = played_index + played_skip;
            if (ti < target_count && pi < played_count &&
                target[ti].midi == played[pi].midi) {
                *out_target = ti;
                *out_played = pi;
                return true;
            }
        }
    }
    return false;
}

static int align_greedy(const score_note_t *target,
                        size_t target_count,
                        const performance_note_t *played,
                        size_t played_count,
                        const alignment_context_t *context,
                        alignment_step_t *steps,
                        size_t capacity)
{
    size_t ti = 0;
    size_t pi = 0;
    size_t count = 0;
    while ((ti < target_count || pi < played_count) && count < capacity) {
        if (ti >= target_count) {
            steps[count++] = (alignment_step_t){-1, (int)pi++, ALIGN_EXTRA};
            continue;
        }
        if (pi >= played_count) {
            steps[count++] = (alignment_step_t){(int)ti++, -1, ALIGN_MISSING};
            continue;
        }
        float current = match_cost(&target[ti], (int)ti, &played[pi],
                                   (int)pi, context, NULL);
        if (target[ti].midi == played[pi].midi && current < SCORE_INF_COST) {
            steps[count++] = (alignment_step_t){(int)ti++, (int)pi++, ALIGN_DIAG};
            continue;
        }
        size_t anchor_target = ti;
        size_t anchor_played = pi;
        if (find_resync(target, target_count, played, played_count,
                        ti, pi, &anchor_target, &anchor_played)) {
            if (anchor_target > ti) {
                steps[count++] = (alignment_step_t){(int)ti++, -1, ALIGN_MISSING};
                continue;
            }
            if (anchor_played > pi) {
                steps[count++] = (alignment_step_t){-1, (int)pi++, ALIGN_EXTRA};
                continue;
            }
        }
        float skip_target = ti + 1 < target_count
                                ? match_cost(&target[ti + 1], (int)(ti + 1),
                                             &played[pi], (int)pi,
                                             context, NULL) + SCORE_MISSING_COST
                                : SCORE_MISSING_COST + SCORE_EXTRA_COST;
        float skip_played = pi + 1 < played_count
                                ? match_cost(&target[ti], (int)ti,
                                             &played[pi + 1], (int)(pi + 1),
                                             context, NULL) + SCORE_EXTRA_COST
                                : SCORE_MISSING_COST + SCORE_EXTRA_COST;
        if (current >= SCORE_INF_COST || skip_target < current ||
            skip_played < current) {
            if (skip_target <= skip_played) {
                steps[count++] = (alignment_step_t){(int)ti++, -1, ALIGN_MISSING};
            } else {
                steps[count++] = (alignment_step_t){-1, (int)pi++, ALIGN_EXTRA};
            }
        } else {
            steps[count++] = (alignment_step_t){(int)ti++, (int)pi++, ALIGN_DIAG};
        }
    }
    return (int)count;
}

static int align_notes(const score_note_t *target,
                       size_t target_count,
                       const performance_note_t *played,
                       size_t played_count,
                       const alignment_context_t *context,
                       alignment_step_t *steps,
                       size_t capacity)
{
    size_t rows = target_count + 1U;
    size_t columns = played_count + 1U;
    if (rows > SIZE_MAX / columns || rows * columns > SCORE_DP_MAX_CELLS) {
        return align_greedy(target, target_count, played, played_count,
                            context, steps, capacity);
    }

    size_t cells = rows * columns;
    uint8_t *operations = preferred_alloc(cells, true);
    uint16_t *previous = preferred_alloc(columns * sizeof(*previous), true);
    uint16_t *current = preferred_alloc(columns * sizeof(*current), true);
    alignment_step_t *reverse = preferred_alloc(capacity * sizeof(*reverse), false);
    if (operations == NULL || previous == NULL || current == NULL || reverse == NULL) {
        heap_caps_free(operations);
        heap_caps_free(previous);
        heap_caps_free(current);
        heap_caps_free(reverse);
        return align_greedy(target, target_count, played, played_count,
                            context, steps, capacity);
    }

    uint16_t missing = scaled_cost(SCORE_MISSING_COST);
    uint16_t extra = scaled_cost(SCORE_EXTRA_COST);
    for (size_t j = 1; j <= played_count; ++j) {
        previous[j] = add_cost(previous[j - 1], extra);
        operations[j] = ALIGN_EXTRA;
    }
    for (size_t i = 1; i <= target_count; ++i) {
        current[0] = add_cost(previous[0], missing);
        operations[i * columns] = ALIGN_MISSING;
        for (size_t j = 1; j <= played_count; ++j) {
            uint16_t diagonal = add_cost(
                previous[j - 1], scaled_cost(match_cost(&target[i - 1],
                                                        (int)(i - 1),
                                                        &played[j - 1],
                                                        (int)(j - 1),
                                                        context, NULL)));
            uint16_t omit_target = add_cost(previous[j], missing);
            uint16_t omit_played = add_cost(current[j - 1], extra);
            uint16_t best = diagonal;
            uint8_t op = ALIGN_DIAG;
            if (omit_target < best) {
                best = omit_target;
                op = ALIGN_MISSING;
            }
            if (omit_played < best) {
                best = omit_played;
                op = ALIGN_EXTRA;
            }
            current[j] = best;
            operations[i * columns + j] = op;
        }
        uint16_t *swap = previous;
        previous = current;
        current = swap;
    }

    size_t i = target_count;
    size_t j = played_count;
    size_t reverse_count = 0;
    while ((i > 0 || j > 0) && reverse_count < capacity) {
        uint8_t op = operations[i * columns + j];
        if (i > 0 && j > 0 && op == ALIGN_DIAG) {
            reverse[reverse_count++] = (alignment_step_t){
                (int)(i - 1), (int)(j - 1), ALIGN_DIAG};
            --i;
            --j;
        } else if (i > 0 && (j == 0 || op == ALIGN_MISSING)) {
            reverse[reverse_count++] = (alignment_step_t){
                (int)(i - 1), -1, ALIGN_MISSING};
            --i;
        } else if (j > 0) {
            reverse[reverse_count++] = (alignment_step_t){
                -1, (int)(j - 1), ALIGN_EXTRA};
            --j;
        }
    }
    size_t count = 0;
    while (reverse_count > 0 && count < capacity) {
        steps[count++] = reverse[--reverse_count];
    }
    heap_caps_free(operations);
    heap_caps_free(previous);
    heap_caps_free(current);
    heap_caps_free(reverse);
    return (int)count;
}

typedef struct {
    float tempo_scale;
    float start_offset_ms;
    float previous_target_start_ms;
    float previous_played_start_ms;
    bool have_previous;
    bool have_offset;
} timing_estimator_t;

/*
 * Incremental timing estimator. Only exact-pitch matches whose played
 * duration is within 40% of the current model are allowed to update tempo.
 */
static void estimate_timing_update(timing_estimator_t *estimator,
                                   const score_note_t *target,
                                   const performance_note_t *played)
{
    float expected_duration =
        max_float(1.0f, (float)target->duration_ms *
                            estimator->tempo_scale);
    float duration_error_ratio =
        fabsf((float)played->duration_ms / expected_duration - 1.0f);
    if (duration_error_ratio > 0.40f) return;

    float duration_scale =
        (float)played->duration_ms /
        max_float(1.0f, (float)target->duration_ms);
    float sample = duration_scale;
    if (estimator->have_previous) {
        float target_delta =
            (float)target->start_ms - estimator->previous_target_start_ms;
        float played_delta =
            (float)played->start_ms - estimator->previous_played_start_ms;
        if (target_delta >= 50.0f && played_delta > 0.0f) {
            float onset_scale = played_delta / target_delta;
            if (onset_scale >= SCORE_TEMPO_MIN &&
                onset_scale <= SCORE_TEMPO_MAX) {
                sample = 0.5f * (sample + onset_scale);
            }
        }
    }

    if (sample >= SCORE_TEMPO_MIN && sample <= SCORE_TEMPO_MAX) {
        estimator->tempo_scale = clamp_float(
            0.85f * estimator->tempo_scale + 0.15f * sample,
            SCORE_TEMPO_MIN, SCORE_TEMPO_MAX);
    }

    float offset_sample =
        (float)played->start_ms -
        (float)target->start_ms * estimator->tempo_scale;
    if (!estimator->have_offset) {
        estimator->start_offset_ms = offset_sample;
        estimator->have_offset = true;
    } else {
        estimator->start_offset_ms =
            0.85f * estimator->start_offset_ms + 0.15f * offset_sample;
    }

    estimator->previous_target_start_ms = (float)target->start_ms;
    estimator->previous_played_start_ms = (float)played->start_ms;
    estimator->have_previous = true;
}

static void estimate_timing(const score_note_t *target,
                            const performance_note_t *played,
                            const alignment_step_t *steps,
                            size_t step_count,
                            float fallback_offset,
                            float *out_tempo,
                            float *out_offset)
{
    timing_estimator_t estimator = {
        .tempo_scale = 1.0f,
        .start_offset_ms = fallback_offset,
    };
    for (size_t index = 0; index < step_count; ++index) {
        const alignment_step_t *step = &steps[index];
        if (step->op != ALIGN_DIAG ||
            target[step->target_index].midi != played[step->played_index].midi) {
            continue;
        }
        estimate_timing_update(&estimator,
                               &target[step->target_index],
                               &played[step->played_index]);
    }
    *out_tempo = estimator.tempo_scale;
    *out_offset = estimator.have_offset
                      ? estimator.start_offset_ms
                      : fallback_offset;
}

static float smooth_score(float error, float excellent, float okay, float bad)
{
    error = fabsf(error);
    if (error <= excellent) {
        return 100.0f;
    }
    if (error >= bad) {
        return 0.0f;
    }
    if (error <= okay) {
        float x = (error - excellent) / max_float(0.001f, okay - excellent);
        return 100.0f - 25.0f * x * x;
    }
    float x = (error - okay) / max_float(0.001f, bad - okay);
    return 75.0f * (1.0f - x * x);
}

static bool json_reserve(json_builder_t *builder, size_t needed)
{
    if (builder->failed || needed > SCORE_JSON_MAX_BYTES) {
        builder->failed = true;
        return false;
    }
    if (needed <= builder->capacity) {
        return true;
    }
    size_t capacity = builder->capacity == 0
                          ? SCORE_JSON_INITIAL_BYTES
                          : builder->capacity;
    while (capacity < needed && capacity < SCORE_JSON_MAX_BYTES) {
        capacity *= 2U;
    }
    if (capacity > SCORE_JSON_MAX_BYTES) {
        capacity = SCORE_JSON_MAX_BYTES;
    }
    if (capacity < needed) {
        builder->failed = true;
        return false;
    }
    char *next = preferred_alloc(capacity, false);
    if (next == NULL) {
        builder->failed = true;
        return false;
    }
    if (builder->data != NULL && builder->length > 0) {
        memcpy(next, builder->data, builder->length);
    }
    heap_caps_free(builder->data);
    builder->data = next;
    builder->capacity = capacity;
    builder->data[builder->length] = '\0';
    return true;
}

static bool json_append(json_builder_t *builder, const char *text)
{
    size_t length = strlen(text);
    if (!json_reserve(builder, builder->length + length + 1U)) {
        return false;
    }
    memcpy(builder->data + builder->length, text, length + 1U);
    builder->length += length;
    return true;
}

static bool json_appendf(json_builder_t *builder, const char *format, ...)
{
    while (!builder->failed) {
        if (!json_reserve(builder, builder->length + 256U)) {
            return false;
        }
        va_list arguments;
        va_start(arguments, format);
        int written = vsnprintf(builder->data + builder->length,
                                builder->capacity - builder->length,
                                format, arguments);
        va_end(arguments);
        if (written < 0) {
            builder->failed = true;
            return false;
        }
        size_t needed = builder->length + (size_t)written + 1U;
        if (needed <= builder->capacity) {
            builder->length += (size_t)written;
            return true;
        }
        if (!json_reserve(builder, needed)) {
            return false;
        }
    }
    return false;
}

static bool json_append_escaped(json_builder_t *builder, const char *text)
{
    if (!json_append(builder, "\"")) {
        return false;
    }
    const unsigned char *cursor = (const unsigned char *)(text != NULL ? text : "");
    while (*cursor != '\0' && !builder->failed) {
        switch (*cursor) {
            case '\"':
                json_append(builder, "\\\"");
                break;
            case '\\':
                json_append(builder, "\\\\");
                break;
            case '\b':
                json_append(builder, "\\b");
                break;
            case '\f':
                json_append(builder, "\\f");
                break;
            case '\n':
                json_append(builder, "\\n");
                break;
            case '\r':
                json_append(builder, "\\r");
                break;
            case '\t':
                json_append(builder, "\\t");
                break;
            default:
                if (*cursor < 0x20U) {
                    json_appendf(builder, "\\u%04x", *cursor);
                } else {
                    if (!json_reserve(builder, builder->length + 2U)) {
                        return false;
                    }
                    builder->data[builder->length++] = (char)*cursor;
                    builder->data[builder->length] = '\0';
                }
                break;
        }
        ++cursor;
    }
    return json_append(builder, "\"");
}

static const char *detail_result(bool pitch_ok,
                                 float timing_error_ms,
                                 float duration_error_ratio)
{
    bool rhythm_bad = timing_error_ms > 150.0f;
    bool duration_bad = duration_error_ratio > 0.25f;
    if (pitch_ok && !rhythm_bad && !duration_bad) {
        return "correct";
    }
    if (!pitch_ok && rhythm_bad) {
        return "pitch_and_rhythm_error";
    }
    if (!pitch_ok) {
        return "pitch_error";
    }
    if (rhythm_bad) {
        return "rhythm_error";
    }
    return "duration_error";
}

esp_err_t score_engine_build_midi_result_json(
    const score_document_t *score,
    performance_snapshot_t *performance,
    char **out_json,
    size_t *out_length,
    char *error,
    size_t error_length)
{
    if (out_json != NULL) {
        *out_json = NULL;
    }
    if (out_length != NULL) {
        *out_length = 0;
    }
    if (error != NULL && error_length > 0) {
        error[0] = '\0';
    }
    if (score == NULL || performance == NULL || out_json == NULL ||
        out_length == NULL || score->notes == NULL || score->note_count == 0 ||
        (performance->count > 0 && performance->notes == NULL) ||
        (performance->input_source != INPUT_SOURCE_USB_MIDI &&
         performance->input_source != INPUT_SOURCE_AUDIO_S3)) {
        set_error(error, error_length, "invalid_midi_scoring_input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *const result_source =
        performance->input_source == INPUT_SOURCE_AUDIO_S3
            ? "AUDIO_S3"
            : "USB_MIDI";
    const char *const input_source = input_source_name(performance->input_source);

    if (performance->count > 1) {
        qsort(performance->notes, performance->count,
              sizeof(*performance->notes), compare_performance_notes);
    }
    size_t maximum_steps = score->note_count + performance->count;
    alignment_step_t *rough_steps = preferred_alloc(
        maximum_steps * sizeof(*rough_steps), false);
    alignment_step_t *steps = preferred_alloc(
        maximum_steps * sizeof(*steps), false);
    if (rough_steps == NULL || steps == NULL) {
        heap_caps_free(rough_steps);
        heap_caps_free(steps);
        set_error(error, error_length, "score_alignment_allocation_failed");
        return ESP_ERR_NO_MEM;
    }

    float fallback_offset = 0.0f;
    if (performance->count > 0) {
        fallback_offset = (float)performance->notes[0].start_ms -
                          (float)score->notes[0].start_ms;
    }
    alignment_context_t rough_context = {
        .tempo_scale = 1.0f,
        .start_offset_ms = 0.0f,
        .gate_scale = SCORE_ROUGH_GATE_SCALE,
        .sequence_only = true,
    };
    int rough_count = align_notes(score->notes, score->note_count,
                                  performance->notes, performance->count,
                                  &rough_context, rough_steps, maximum_steps);
    if (rough_count < 0) {
        heap_caps_free(rough_steps);
        heap_caps_free(steps);
        set_error(error, error_length, "score_rough_alignment_failed");
        return ESP_FAIL;
    }

    float tempo_scale = 1.0f;
    float start_offset_ms = fallback_offset;
    estimate_timing(score->notes, performance->notes, rough_steps,
                    (size_t)rough_count, fallback_offset,
                    &tempo_scale, &start_offset_ms);
    heap_caps_free(rough_steps);

    alignment_context_t fine_context = {
        .tempo_scale = tempo_scale,
        .start_offset_ms = start_offset_ms,
        .gate_scale = SCORE_FINE_GATE_SCALE,
        .sequence_only = false,
    };
    int step_count = align_notes(score->notes, score->note_count,
                                 performance->notes, performance->count,
                                 &fine_context, steps, maximum_steps);
    if (step_count < 0) {
        heap_caps_free(steps);
        set_error(error, error_length, "score_fine_alignment_failed");
        return ESP_FAIL;
    }

    size_t matched_count = 0;
    size_t missing_count = 0;
    size_t extra_count = 0;
    size_t correct_pitch_count = 0;
    float rhythm_sum = 0.0f;
    bool have_previous_match = false;
    float previous_target_start = 0.0f;
    float previous_played_start = 0.0f;

    json_builder_t details = {0};
    for (int index = 0; index < step_count; ++index) {
        alignment_step_t *step = &steps[index];
        if (index > 0) {
            json_append(&details, ",");
        }
        if (step->op == ALIGN_MISSING) {
            const score_note_t *target = &score->notes[step->target_index];
            ++missing_count;
            json_appendf(
                &details,
                "{\"index\":%d,\"ref_index\":%d,\"played_index\":-1,"
                "\"expected_midi\":%u,\"played_midi\":-1,"
                "\"expected_start_ms\":%u,\"played_start_ms\":-1,"
                "\"aligned_played_start_ms\":-1,"
                "\"timing_error_ms\":0,\"expected_duration_ms\":%u,"
                "\"played_duration_ms\":0,\"pitch_error_semitones\":0,"
                "\"velocity\":0,\"channel\":-1,\"source\":\"%s\"," 
                "\"time_error\":0,\"pitch_error\":0,\"duration_error\":0,"
                "\"target_midi\":%u,\"target_start\":%.3f,"
                "\"target_duration\":%.3f,\"played_start\":-1,"
                "\"aligned_played_start\":-1,"
                "\"played_duration\":0,\"result\":\"missing\"}",
                index + 1, step->target_index + 1, target->midi,
                target->start_ms, target->duration_ms, result_source,
                target->midi,
                (double)target->start_ms / 1000.0,
                (double)target->duration_ms / 1000.0);
            continue;
        }
        if (step->op == ALIGN_EXTRA) {
            const performance_note_t *played =
                &performance->notes[step->played_index];
            float aligned_played_start_ms =
                ((float)played->start_ms - start_offset_ms) /
                max_float(0.001f, tempo_scale);
            ++extra_count;
            json_appendf(
                &details,
                "{\"index\":%d,\"ref_index\":-1,\"played_index\":%d,"
                "\"expected_midi\":-1,\"played_midi\":%u,"
                "\"expected_start_ms\":-1,\"played_start_ms\":%u,"
                "\"aligned_played_start_ms\":%.0f,"
                "\"timing_error_ms\":0,\"expected_duration_ms\":0,"
                "\"played_duration_ms\":%u,\"pitch_error_semitones\":0,"
                "\"velocity\":%u,\"channel\":%u,\"source\":\"%s\"," 
                "\"time_error\":0,\"pitch_error\":0,\"duration_error\":0,"
                "\"target_midi\":-1,\"target_start\":-1,"
                "\"target_duration\":0,\"played_start\":%.3f,"
                "\"aligned_played_start\":%.3f,"
                "\"played_duration\":%.3f,\"result\":\"extra\"}",
                index + 1, step->played_index + 1, played->midi,
                played->start_ms, aligned_played_start_ms,
                played->duration_ms, played->velocity,
                played->channel, result_source,
                (double)played->start_ms / 1000.0,
                (double)aligned_played_start_ms / 1000.0,
                (double)played->duration_ms / 1000.0);
            continue;
        }

        const score_note_t *target = &score->notes[step->target_index];
        const performance_note_t *played =
            &performance->notes[step->played_index];
        match_metrics_t metrics = {0};
        match_cost(target, step->target_index, played, step->played_index,
                   &fine_context, &metrics);
        float aligned_played_start_ms =
            ((float)played->start_ms - start_offset_ms) /
            max_float(0.001f, tempo_scale);
        bool pitch_ok = target->midi == played->midi;
        if (pitch_ok) {
            ++correct_pitch_count;
        }
        float rhythm_score;
        if (have_previous_match) {
            float target_interval = (float)target->start_ms -
                                    previous_target_start;
            float played_interval = (float)played->start_ms -
                                    previous_played_start;
            float interval_error = fabsf(played_interval -
                                         target_interval * tempo_scale);
            rhythm_score = smooth_score(interval_error / 1000.0f,
                                        0.08f, 0.30f, 1.00f);
        } else {
            rhythm_score = smooth_score(metrics.time_error_ms / 1000.0f,
                                        0.15f, 0.55f, 1.35f);
        }
        rhythm_sum += rhythm_score;
        ++matched_count;
        have_previous_match = true;
        previous_target_start = (float)target->start_ms;
        previous_played_start = (float)played->start_ms;

        const char *result = detail_result(pitch_ok, metrics.time_error_ms,
                                           metrics.duration_error_ratio);
        json_appendf(
            &details,
            "{\"index\":%d,\"ref_index\":%d,\"played_index\":%d,"
            "\"expected_midi\":%u,\"played_midi\":%u,"
            "\"expected_start_ms\":%u,\"played_start_ms\":%u,"
            "\"aligned_played_start_ms\":%.0f,"
            "\"timing_error_ms\":%.0f,\"expected_duration_ms\":%u,"
            "\"played_duration_ms\":%u,\"pitch_error_semitones\":%d,"
            "\"velocity\":%u,\"channel\":%u,\"source\":\"%s\"," 
            "\"time_error\":%.3f,\"pitch_error\":%d,"
            "\"duration_error\":%.3f,"
            "\"target_midi\":%u,\"target_start\":%.3f,"
            "\"target_duration\":%.3f,\"played_start\":%.3f,"
            "\"aligned_played_start\":%.3f,"
            "\"played_duration\":%.3f,\"result\":\"%s\"}",
            index + 1, step->target_index + 1, step->played_index + 1,
            target->midi, played->midi, target->start_ms, played->start_ms,
            aligned_played_start_ms, metrics.time_error_ms,
            target->duration_ms, played->duration_ms,
            metrics.pitch_error, played->velocity, played->channel,
            result_source,
            (double)metrics.time_error_ms / 1000.0, metrics.pitch_error,
            metrics.duration_error_ratio, target->midi,
            (double)target->start_ms / 1000.0,
            (double)target->duration_ms / 1000.0,
            (double)played->start_ms / 1000.0,
            (double)aligned_played_start_ms / 1000.0,
            (double)played->duration_ms / 1000.0, result);
    }
    heap_caps_free(steps);
    if (details.failed) {
        heap_caps_free(details.data);
        set_error(error, error_length, "score_result_details_too_large");
        return ESP_ERR_NO_MEM;
    }

    float pitch_score = matched_count > 0
                            ? (float)correct_pitch_count * 100.0f /
                                  (float)matched_count
                            : 0.0f;
    float rhythm_score = matched_count > 0
                             ? rhythm_sum / (float)matched_count
                             : 0.0f;
    float complete_score = (float)matched_count * 100.0f /
                           (float)score->note_count;
    complete_score = clamp_float(complete_score, 0.0f, 100.0f);
    float total_score = 0.55f * pitch_score +
                        0.35f * rhythm_score +
                        0.10f * complete_score;
    total_score = clamp_float(total_score, 0.0f, 100.0f);
    size_t wrong_count = matched_count - correct_pitch_count;

    json_builder_t header = {0};
    json_append(&header,
                "{\"ok\":true,\"ready\":true,\"state\":\"ready\",\"title\":");
    json_append_escaped(&header, score->title);
    json_appendf(
        &header,
        ",\"input_source\":\"%s\",\"profile\":\"midi_strict\"," 
        "\"target_count\":%u,\"played_count\":%u,"
        "\"matched_count\":%u,\"missing_count\":%u,\"extra_count\":%u,"
        "\"wrong_count\":%u,"
        "\"pitch_score\":%.1f,\"rhythm_score\":%.1f,"
        "\"complete_score\":%.1f,\"total_score\":%.1f,"
        "\"tempo_scale\":%.4f,\"start_offset\":%.4f,"
        "\"start_offset_ms\":%.0f,"
        "\"alignment_method\":\"midi_sequence_then_offset_tempo\","
        "\"rhythm_method\":\"relative_interval_rhythm\","
        "\"audio_frame_recovery\":false,\"octave_correction\":false,"
        "\"audio_stable_frame_merge\":false,\"details\":[",
        input_source, (unsigned)score->note_count,
        (unsigned)performance->count,
        (unsigned)matched_count, (unsigned)missing_count,
        (unsigned)extra_count, (unsigned)wrong_count,
        pitch_score, rhythm_score, complete_score,
        total_score, tempo_scale, start_offset_ms / 1000.0f,
        start_offset_ms);
    if (header.failed || header.data == NULL ||
        !json_reserve(&details,
                      header.length + details.length + sizeof("]}"))) {
        heap_caps_free(header.data);
        heap_caps_free(details.data);
        set_error(error, error_length, "score_result_allocation_failed");
        return ESP_ERR_NO_MEM;
    }
    memmove(details.data + header.length, details.data, details.length);
    memcpy(details.data, header.data, header.length);
    details.length += header.length;
    memcpy(details.data + details.length, "]}", sizeof("]}"));
    details.length += 2U;
    heap_caps_free(header.data);

    *out_json = details.data;
    *out_length = details.length;
    return ESP_OK;
}

void score_engine_result_free(char *json)
{
    heap_caps_free(json);
}
