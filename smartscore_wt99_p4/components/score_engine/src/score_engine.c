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
#define SCORE_AUDIO_CONFIDENCE_MIN 0.50f
#define SCORE_AUDIO_CONFIDENCE_HIGH 0.75f
#define SCORE_START_ANCHOR_PLAYED_WINDOW 3U
#define SCORE_TEMPO_BOOTSTRAP_WINDOW 10U
#define SCORE_TEMPO_SAMPLE_CAPACITY 16U
#define SCORE_BEGINNER_GREEN_PITCH_SEMITONES 2
#define SCORE_BEGINNER_RHYTHM_EXCELLENT_BEATS 0.15f
#define SCORE_BEGINNER_RHYTHM_OKAY_BEATS 0.45f
#define SCORE_BEGINNER_RHYTHM_BAD_BEATS 0.95f

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
    bool valid;
    size_t target_index;
    size_t played_index;
} start_anchor_t;

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

static float beginner_pitch_alignment_cost(int pitch_error)
{
    const int distance = abs(pitch_error);
    switch (distance) {
    case 0: return 0.0f;
    case 1: return 1.0f;
    case 2: return 2.0f;
    case 3: return 4.0f;
    case 4: return 6.0f;
    case 5: return 7.5f;
    default: return SCORE_BAD_MATCH_COST;
    }
}

/* Pitch is scored continuously so a detector wobble or a nearby beginner
 * note is not treated the same as a completely unrelated pitch. */
static float beginner_pitch_credit(int pitch_error)
{
    const int distance = abs(pitch_error);
    switch (distance) {
    case 0: return 100.0f;
    case 1: return 98.0f;
    case 2: return 94.0f;
    case 3: return 86.0f;
    case 4: return 76.0f;
    case 5: return 64.0f;
    case 6: return 50.0f;
    case 7: return 36.0f;
    case 8: return 24.0f;
    case 9: return 16.0f;
    case 10: return 8.0f;
    default: return 0.0f;
    }
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
        float cost = beginner_pitch_alignment_cost(pitch_error);
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

    const int pitch_distance = abs(pitch_error);
    float pitch_component =
        beginner_pitch_alignment_cost(pitch_error);
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
    float interval_component = clamp_float(interval_error * 1.2f, 0.0f, 10.0f);
    float cost = 0.65f * pitch_component +
                 0.25f * time_component +
                 0.10f * interval_component;
    if (pitch_distance >= 6) {
        cost = max_float(cost, 7.5f);
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

static start_anchor_t find_start_anchor(const score_note_t *target,
                                        size_t target_count,
                                        const performance_note_t *played,
                                        size_t played_count)
{
    start_anchor_t anchor = {0};
    if (target_count == 0 || played_count == 0) return anchor;

    const size_t played_window = played_count < SCORE_START_ANCHOR_PLAYED_WINDOW
                                     ? played_count
                                     : SCORE_START_ANCHOR_PLAYED_WINDOW;

    /* The score origin is always target note zero. A few leading played
     * observations may be skipped as noise, but the algorithm can never jump
     * into a later repeated phrase. Near pitches win over later exact pitches
     * when their total beginner alignment cost is lower. */
    float best_cost = SCORE_INF_COST;
    for (size_t played_index = 0; played_index < played_window;
         ++played_index) {
        const int pitch_error =
            (int)played[played_index].midi - (int)target[0].midi;
        const float edit_cost =
            (float)played_index * SCORE_EXTRA_COST +
            beginner_pitch_alignment_cost(pitch_error);
        if (edit_cost >= best_cost) continue;
        best_cost = edit_cost;
        anchor = (start_anchor_t) {
            .valid = true,
            .target_index = 0,
            .played_index = played_index,
        };
    }
    return anchor;
}

static int align_notes_start_locked(const score_note_t *target,
                                    size_t target_count,
                                    const performance_note_t *played,
                                    size_t played_count,
                                    const alignment_context_t *context,
                                    const start_anchor_t *anchor,
                                    alignment_step_t *steps,
                                    size_t capacity)
{
    if (anchor == NULL || !anchor->valid) {
        return align_notes(target, target_count, played, played_count,
                           context, steps, capacity);
    }
    if (anchor->target_index >= target_count ||
        anchor->played_index >= played_count) {
        return -1;
    }

    size_t count = 0;
    for (size_t index = 0; index < anchor->target_index; ++index) {
        if (count >= capacity) return -1;
        steps[count++] = (alignment_step_t) {
            .target_index = (int)index,
            .played_index = -1,
            .op = ALIGN_MISSING,
        };
    }
    for (size_t index = 0; index < anchor->played_index; ++index) {
        if (count >= capacity) return -1;
        steps[count++] = (alignment_step_t) {
            .target_index = -1,
            .played_index = (int)index,
            .op = ALIGN_EXTRA,
        };
    }
    if (count >= capacity) return -1;
    steps[count++] = (alignment_step_t) {
        .target_index = (int)anchor->target_index,
        .played_index = (int)anchor->played_index,
        .op = ALIGN_DIAG,
    };

    const size_t target_offset = anchor->target_index + 1U;
    const size_t played_offset = anchor->played_index + 1U;
    const size_t target_remaining = target_count - target_offset;
    const size_t played_remaining = played_count - played_offset;
    if (target_remaining == 0 && played_remaining == 0) {
        return (int)count;
    }

    const int suffix_count = align_notes(
        target + target_offset, target_remaining,
        played + played_offset, played_remaining,
        context, steps + count, capacity - count);
    if (suffix_count < 0) return suffix_count;
    for (int index = 0; index < suffix_count; ++index) {
        alignment_step_t *step = &steps[count + (size_t)index];
        if (step->target_index >= 0) {
            step->target_index += (int)target_offset;
        }
        if (step->played_index >= 0) {
            step->played_index += (int)played_offset;
        }
    }
    return (int)count + suffix_count;
}

static int compare_float_values(const void *left, const void *right)
{
    const float a = *(const float *)left;
    const float b = *(const float *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

/* Estimate only tempo from the bounded opening alignment. The start offset
 * always remains pinned to the selected anchor, even when its pitch is a
 * substitution. Later repeated phrases can therefore never move the origin. */
static void estimate_timing_from_anchor(
    const score_note_t *target,
    const performance_note_t *played,
    const alignment_step_t *steps,
    size_t step_count,
    const start_anchor_t *anchor,
    float fallback_offset,
    float *out_tempo,
    float *out_offset,
    size_t *out_sample_count)
{
    *out_tempo = 1.0f;
    *out_offset = fallback_offset;
    *out_sample_count = 0;
    if (anchor == NULL || !anchor->valid) return;

    const score_note_t *anchor_target = &target[anchor->target_index];
    const performance_note_t *anchor_played = &played[anchor->played_index];
    float samples[SCORE_TEMPO_SAMPLE_CAPACITY] = {0};
    size_t sample_count = 0;
    for (size_t index = 0; index < step_count; ++index) {
        const alignment_step_t *step = &steps[index];
        if (step->op != ALIGN_DIAG || step->target_index < 0 ||
            step->played_index < 0 ||
            (size_t)step->target_index == anchor->target_index ||
            target[step->target_index].midi != played[step->played_index].midi)
            continue;
        const performance_note_t *sample_played =
            &played[step->played_index];
        if (sample_played->source == INPUT_SOURCE_AUDIO_S3 &&
            sample_played->confidence < SCORE_AUDIO_CONFIDENCE_HIGH)
            continue;

        const float target_delta =
            (float)target[step->target_index].start_ms -
            (float)anchor_target->start_ms;
        const float played_delta =
            (float)sample_played->start_ms -
            (float)anchor_played->start_ms;
        if (target_delta < 50.0f || played_delta <= 0.0f) continue;
        const float sample = played_delta / target_delta;
        if (sample < SCORE_TEMPO_MIN || sample > SCORE_TEMPO_MAX) continue;
        if (sample_count < SCORE_TEMPO_SAMPLE_CAPACITY) {
            samples[sample_count++] = sample;
        }
    }

    if (sample_count > 0) {
        qsort(samples, sample_count, sizeof(samples[0]),
              compare_float_values);
        const size_t middle = sample_count / 2U;
        *out_tempo = sample_count % 2U != 0U
                         ? samples[middle]
                         : 0.5f * (samples[middle - 1U] + samples[middle]);
        *out_tempo = clamp_float(*out_tempo,
                                 SCORE_TEMPO_MIN, SCORE_TEMPO_MAX);
    }
    *out_offset = (float)anchor_played->start_ms -
                  (float)anchor_target->start_ms * (*out_tempo);
    *out_sample_count = sample_count;
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

static bool performance_note_is_reliable(const performance_note_t *note)
{
    return note->source != INPUT_SOURCE_AUDIO_S3 ||
           note->confidence >= SCORE_AUDIO_CONFIDENCE_MIN;
}

static bool performance_note_is_uncertain(const performance_note_t *note)
{
    return note->source == INPUT_SOURCE_AUDIO_S3 &&
           note->confidence < SCORE_AUDIO_CONFIDENCE_MIN;
}

static float performance_note_confidence(const performance_note_t *note)
{
    if (note->source != INPUT_SOURCE_AUDIO_S3) {
        return 1.0f;
    }
    return clamp_float(note->confidence, 0.0f, 1.0f);
}

static const char *score_level(float score)
{
    if (score >= 95.0f) return "非常棒";
    if (score >= 85.0f) return "表现很好";
    if (score >= 70.0f) return "基本掌握";
    if (score >= 50.0f) return "继续练习";
    return "建议分段练习";
}

static bool extra_step_is_retry(const alignment_step_t *steps,
                                size_t step_count,
                                size_t index,
                                const performance_note_t *played,
                                float beat_ms)
{
    if (steps[index].op != ALIGN_EXTRA) return false;
    const performance_note_t *extra = &played[steps[index].played_index];
    for (size_t next = index + 1; next < step_count; ++next) {
        if (steps[next].op == ALIGN_MISSING) continue;
        if (steps[next].op == ALIGN_EXTRA) continue;
        const performance_note_t *matched =
            &played[steps[next].played_index];
        const int32_t gap_ms =
            (int32_t)(matched->start_ms - extra->start_ms);
        return gap_ms >= 0 && (float)gap_ms <= 1.5f * beat_ms;
    }
    return false;
}

static int find_uncertain_note(const performance_snapshot_t *performance,
                               bool *used,
                               float expected_start_ms,
                               float gate_ms)
{
    int best = -1;
    float best_error = gate_ms;
    for (size_t index = 0; index < performance->count; ++index) {
        const performance_note_t *note = &performance->notes[index];
        if ((used != NULL && used[index]) ||
            !performance_note_is_uncertain(note)) {
            continue;
        }
        const float error = fabsf((float)note->start_ms - expected_start_ms);
        if (error <= best_error) {
            best = (int)index;
            best_error = error;
        }
    }
    if (best >= 0 && used != NULL) used[best] = true;
    return best;
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

    const bool is_audio =
        performance->input_source == INPUT_SOURCE_AUDIO_S3;
    const char *const result_source = is_audio ? "AUDIO_S3" : "USB_MIDI";
    const char *const input_source =
        input_source_name(performance->input_source);

    if (performance->count > 1) {
        qsort(performance->notes, performance->count,
              sizeof(*performance->notes), compare_performance_notes);
    }

    size_t reliable_count = 0;
    size_t derived_uncertain_count = 0;
    float derived_confidence_sum = 0.0f;
    for (size_t index = 0; index < performance->count; ++index) {
        const performance_note_t *note = &performance->notes[index];
        derived_confidence_sum += performance_note_confidence(note);
        if (performance_note_is_reliable(note)) {
            ++reliable_count;
        } else {
            ++derived_uncertain_count;
        }
    }

    performance_note_t *reliable_notes = NULL;
    if (reliable_count > 0) {
        reliable_notes = preferred_alloc(
            reliable_count * sizeof(*reliable_notes), false);
        if (reliable_notes == NULL) {
            set_error(error, error_length,
                      "score_reliable_note_allocation_failed");
            return ESP_ERR_NO_MEM;
        }
        size_t output = 0;
        for (size_t index = 0; index < performance->count; ++index) {
            if (performance_note_is_reliable(&performance->notes[index])) {
                reliable_notes[output++] = performance->notes[index];
            }
        }
    }

    const size_t maximum_steps = score->note_count + reliable_count;
    alignment_step_t *rough_steps = preferred_alloc(
        maximum_steps * sizeof(*rough_steps), false);
    alignment_step_t *steps = preferred_alloc(
        maximum_steps * sizeof(*steps), false);
    bool *uncertain_used = performance->count > 0
                               ? preferred_alloc(performance->count,
                                                 true)
                               : NULL;
    if (rough_steps == NULL || steps == NULL ||
        (performance->count > 0 && uncertain_used == NULL)) {
        heap_caps_free(reliable_notes);
        heap_caps_free(rough_steps);
        heap_caps_free(steps);
        heap_caps_free(uncertain_used);
        set_error(error, error_length, "score_alignment_allocation_failed");
        return ESP_ERR_NO_MEM;
    }

    const start_anchor_t start_anchor = find_start_anchor(
        score->notes, score->note_count, reliable_notes, reliable_count);
    float fallback_offset = 0.0f;
    if (start_anchor.valid) {
        fallback_offset =
            (float)reliable_notes[start_anchor.played_index].start_ms -
            (float)score->notes[start_anchor.target_index].start_ms;
    }
    const alignment_context_t rough_context = {
        .tempo_scale = 1.0f,
        .start_offset_ms = 0.0f,
        .gate_scale = SCORE_ROUGH_GATE_SCALE,
        .sequence_only = true,
    };
    const size_t rough_target_count =
        score->note_count < SCORE_TEMPO_BOOTSTRAP_WINDOW
            ? score->note_count : SCORE_TEMPO_BOOTSTRAP_WINDOW;
    const size_t rough_played_count =
        reliable_count < SCORE_TEMPO_BOOTSTRAP_WINDOW
            ? reliable_count : SCORE_TEMPO_BOOTSTRAP_WINDOW;
    const int rough_count = align_notes_start_locked(
        score->notes, rough_target_count,
        reliable_notes, rough_played_count,
        &rough_context, &start_anchor, rough_steps, maximum_steps);
    if (rough_count < 0) {
        heap_caps_free(reliable_notes);
        heap_caps_free(rough_steps);
        heap_caps_free(steps);
        heap_caps_free(uncertain_used);
        set_error(error, error_length, "score_rough_alignment_failed");
        return ESP_FAIL;
    }

    float tempo_scale = 1.0f;
    float start_offset_ms = fallback_offset;
    size_t tempo_sample_count = 0;
    estimate_timing_from_anchor(
        score->notes, reliable_notes, rough_steps,
        (size_t)rough_count, &start_anchor, fallback_offset,
        &tempo_scale, &start_offset_ms, &tempo_sample_count);
    heap_caps_free(rough_steps);

    const alignment_context_t fine_context = {
        .tempo_scale = tempo_scale,
        .start_offset_ms = start_offset_ms,
        .gate_scale = SCORE_FINE_GATE_SCALE,
        .sequence_only = false,
    };
    const int step_count = align_notes_start_locked(
        score->notes, score->note_count, reliable_notes, reliable_count,
        &fine_context, &start_anchor, steps, maximum_steps);
    if (step_count < 0) {
        heap_caps_free(reliable_notes);
        heap_caps_free(steps);
        heap_caps_free(uncertain_used);
        set_error(error, error_length, "score_fine_alignment_failed");
        return ESP_FAIL;
    }

    size_t attempted_count = 0;
    size_t last_target_index = 0;
    uint32_t attempted_end_ms = 0;
    for (int index = 0; index < step_count; ++index) {
        if (steps[index].op == ALIGN_DIAG) {
            const size_t target_index =
                (size_t)steps[index].target_index;
            if (attempted_count == 0 || target_index > last_target_index) {
                last_target_index = target_index;
                attempted_count = target_index + 1U;
                attempted_end_ms =
                    score->notes[target_index].start_ms +
                    score->notes[target_index].duration_ms;
            }
        }
    }

    /* A low-confidence microphone event may still prove that the performer
     * reached a later part of the score. It extends the attempted region but
     * never counts as a correct or wrong note. */
    if (is_audio && attempted_count > 0 &&
        derived_uncertain_count > 0) {
        const float probe_beat_ms =
            (60000.0f / max_float(1.0f, (float)score->bpm)) * tempo_scale;
        const float probe_gate_ms =
            max_float(120.0f, 0.25f * probe_beat_ms + 80.0f);
        for (size_t target_index = attempted_count;
             target_index < score->note_count; ++target_index) {
            const float expected_start = start_offset_ms +
                (float)score->notes[target_index].start_ms * tempo_scale;
            if (find_uncertain_note(performance, NULL, expected_start,
                                    probe_gate_ms) >= 0) {
                attempted_count = target_index + 1U;
                attempted_end_ms =
                    score->notes[target_index].start_ms +
                    score->notes[target_index].duration_ms;
            }
        }
    }

    uint32_t score_end_ms = 1;
    for (size_t index = 0; index < score->note_count; ++index) {
        const uint32_t note_end = score->notes[index].start_ms +
                                  score->notes[index].duration_ms;
        if (note_end > score_end_ms) score_end_ms = note_end;
    }

    size_t correct_count = 0;
    size_t substitution_count = 0;
    size_t missing_count = 0;
    size_t extra_count = 0;
    size_t retry_count = 0;
    size_t uncertain_target_count = 0;
    size_t rhythm_count = 0;
    float pitch_credit_sum = 0.0f;
    float rhythm_sum = 0.0f;
    float continuity_penalty = 0.0f;
    float local_tempo = tempo_scale;
    bool have_previous_match = false;
    bool previous_pitch_ok = false;
    float previous_confidence = 0.0f;
    float previous_target_start = 0.0f;
    float previous_played_start = 0.0f;
    uint16_t previous_uncertainty_ms = 0;
    const float initial_beat_ms =
        (60000.0f / max_float(1.0f, (float)score->bpm)) * tempo_scale;

    json_builder_t details = {0};
    json_append(&details, "");
    size_t detail_count = 0;
    for (int index = 0; index < step_count; ++index) {
        const alignment_step_t *step = &steps[index];
        if (step->op == ALIGN_MISSING &&
            (size_t)step->target_index >= attempted_count) {
            continue;
        }
        if (detail_count++ > 0) json_append(&details, ",");

        if (step->op == ALIGN_MISSING) {
            const score_note_t *target = &score->notes[step->target_index];
            const float expected_start = start_offset_ms +
                (float)target->start_ms * tempo_scale;
            const float uncertain_gate =
                max_float(120.0f, 0.25f * initial_beat_ms + 80.0f);
            const int uncertain_index = find_uncertain_note(
                performance, uncertain_used, expected_start, uncertain_gate);
            if (uncertain_index >= 0) {
                const performance_note_t *uncertain =
                    &performance->notes[uncertain_index];
                ++uncertain_target_count;
                json_appendf(
                    &details,
                    "{\"index\":%u,\"ref_index\":%d,"
                    "\"played_index\":%d,\"expected_midi\":%u,"
                    "\"played_midi\":%u,\"expected_start_ms\":%u,"
                    "\"played_start_ms\":%u,\"confidence\":%.3f,"
                    "\"timing_uncertainty_ms\":%u,"
                    "\"target_midi\":%u,\"target_start\":%.3f,"
                    "\"target_duration\":%.3f,\"played_start\":%.3f,"
                    "\"aligned_played_start\":%.3f,"
                    "\"played_duration\":%.3f,\"time_error\":0,"
                    "\"pitch_error\":0,\"duration_error\":0,"
                    "\"result\":\"uncertain\"}",
                    (unsigned)detail_count, step->target_index + 1,
                    uncertain_index + 1, target->midi, uncertain->midi,
                    target->start_ms, uncertain->start_ms,
                    performance_note_confidence(uncertain),
                    (unsigned)uncertain->timing_uncertainty_ms,
                    target->midi, (double)target->start_ms / 1000.0,
                    (double)target->duration_ms / 1000.0,
                    (double)uncertain->start_ms / 1000.0,
                    (double)target->start_ms / 1000.0,
                    (double)uncertain->duration_ms / 1000.0);
            } else {
                ++missing_count;
                json_appendf(
                    &details,
                    "{\"index\":%u,\"ref_index\":%d,"
                    "\"played_index\":-1,\"expected_midi\":%u,"
                    "\"played_midi\":-1,\"expected_start_ms\":%u,"
                    "\"played_start_ms\":-1,\"confidence\":1.0,"
                    "\"target_midi\":%u,\"target_start\":%.3f,"
                    "\"target_duration\":%.3f,\"played_start\":-1,"
                    "\"aligned_played_start\":-1,"
                    "\"played_duration\":0,\"time_error\":0,"
                    "\"pitch_error\":0,\"duration_error\":0,"
                    "\"result\":\"missing\"}",
                    (unsigned)detail_count, step->target_index + 1,
                    target->midi, target->start_ms, target->midi,
                    (double)target->start_ms / 1000.0,
                    (double)target->duration_ms / 1000.0);
            }
            continue;
        }

        if (step->op == ALIGN_EXTRA) {
            const performance_note_t *played =
                &reliable_notes[step->played_index];
            const bool retry = extra_step_is_retry(
                steps, (size_t)step_count, (size_t)index,
                reliable_notes, initial_beat_ms);
            if (retry) {
                ++retry_count;
            } else {
                ++extra_count;
            }
            const float aligned_start =
                ((float)played->start_ms - start_offset_ms) /
                max_float(0.001f, tempo_scale);
            json_appendf(
                &details,
                "{\"index\":%u,\"ref_index\":-1,"
                "\"played_index\":%d,\"expected_midi\":-1,"
                "\"played_midi\":%u,\"played_start_ms\":%u,"
                "\"aligned_played_start_ms\":%.0f,"
                "\"played_duration_ms\":%u,\"velocity\":%u,"
                "\"confidence\":%.3f,\"frequency_hz\":%.2f,"
                "\"target_midi\":-1,\"target_start\":-1,"
                "\"target_duration\":0,\"played_start\":%.3f,"
                "\"aligned_played_start\":%.3f,"
                "\"played_duration\":%.3f,\"time_error\":0,"
                "\"pitch_error\":0,\"duration_error\":0,"
                "\"result\":\"%s\"}",
                (unsigned)detail_count, step->played_index + 1,
                played->midi, played->start_ms, aligned_start,
                played->duration_ms, played->velocity,
                performance_note_confidence(played), played->frequency_hz,
                (double)played->start_ms / 1000.0,
                (double)aligned_start / 1000.0,
                (double)played->duration_ms / 1000.0,
                retry ? "retry" : "extra");
            continue;
        }

        const score_note_t *target = &score->notes[step->target_index];
        const performance_note_t *played =
            &reliable_notes[step->played_index];
        match_metrics_t metrics = {0};
        match_cost(target, step->target_index, played, step->played_index,
                   &fine_context, &metrics);
        const int pitch_error =
            (int)played->midi - (int)target->midi;
        const bool pitch_ok =
            abs(pitch_error) <= SCORE_BEGINNER_GREEN_PITCH_SEMITONES;
        pitch_credit_sum += beginner_pitch_credit(pitch_error);
        if (pitch_ok) {
            ++correct_count;
        } else {
            ++substitution_count;
        }

        float note_rhythm_score = -1.0f;
        float raw_timing_error_ms = metrics.time_error_ms;
        float effective_timing_error_ms = max_float(
            0.0f, raw_timing_error_ms -
                      (float)played->timing_uncertainty_ms);
        if (have_previous_match) {
            const float target_interval =
                (float)target->start_ms - previous_target_start;
            const float played_interval =
                (float)played->start_ms - previous_played_start;
            if (target_interval > 0.0f) {
                const float predicted_interval = target_interval * local_tempo;
                raw_timing_error_ms = fabsf(played_interval -
                                            predicted_interval);
                const float combined_uncertainty = max_float(
                    (float)previous_uncertainty_ms,
                    (float)played->timing_uncertainty_ms);
                effective_timing_error_ms = max_float(
                    0.0f, raw_timing_error_ms - combined_uncertainty);
                const float beat_ms =
                    (60000.0f /
                     max_float(1.0f, (float)score->bpm)) * local_tempo;
                const float normalized_error =
                    effective_timing_error_ms / max_float(1.0f, beat_ms);
                note_rhythm_score = smooth_score(
                    normalized_error,
                    SCORE_BEGINNER_RHYTHM_EXCELLENT_BEATS,
                    SCORE_BEGINNER_RHYTHM_OKAY_BEATS,
                    SCORE_BEGINNER_RHYTHM_BAD_BEATS);
                rhythm_sum += note_rhythm_score;
                ++rhythm_count;

                const float late_beats =
                    (played_interval - predicted_interval) /
                    max_float(1.0f, beat_ms);
                if (late_beats > 1.5f) {
                    continuity_penalty += 4.0f;
                } else if (late_beats > 0.5f) {
                    continuity_penalty += 1.5f;
                }

                if (pitch_ok && previous_pitch_ok &&
                    performance_note_confidence(played) >=
                        SCORE_AUDIO_CONFIDENCE_HIGH &&
                    previous_confidence >= SCORE_AUDIO_CONFIDENCE_HIGH) {
                    const float sample_tempo =
                        played_interval / target_interval;
                    if (sample_tempo >= SCORE_TEMPO_MIN &&
                        sample_tempo <= SCORE_TEMPO_MAX) {
                        if (fabsf(sample_tempo / max_float(0.01f, local_tempo) -
                                  1.0f) > 0.30f) {
                            continuity_penalty += 1.5f;
                        }
                        local_tempo = clamp_float(
                            0.80f * local_tempo + 0.20f * sample_tempo,
                            SCORE_TEMPO_MIN, SCORE_TEMPO_MAX);
                    }
                }
            }
        }

        have_previous_match = true;
        previous_target_start = (float)target->start_ms;
        previous_played_start = (float)played->start_ms;
        previous_uncertainty_ms = played->timing_uncertainty_ms;
        previous_pitch_ok = pitch_ok;
        previous_confidence = performance_note_confidence(played);

        const char *result = pitch_ok
            ? (note_rhythm_score >= 0.0f && note_rhythm_score < 60.0f
                   ? "rhythm_error"
                   : "correct")
            : (note_rhythm_score >= 0.0f && note_rhythm_score < 60.0f
                   ? "pitch_and_rhythm_error"
                   : "pitch_error");
        const float aligned_start =
            ((float)played->start_ms - start_offset_ms) /
            max_float(0.001f, tempo_scale);
        json_appendf(
            &details,
            "{\"index\":%u,\"ref_index\":%d,\"played_index\":%d,"
            "\"expected_midi\":%u,\"played_midi\":%u,"
            "\"expected_start_ms\":%u,\"played_start_ms\":%u,"
            "\"aligned_played_start_ms\":%.0f,"
            "\"raw_timing_error_ms\":%.0f,\"timing_error_ms\":%.0f,"
            "\"rhythm_note_score\":%.1f,"
            "\"expected_duration_ms\":%u,\"played_duration_ms\":%u,"
            "\"duration_reliable\":%s,\"pitch_error_semitones\":%d,"
            "\"velocity\":%u,\"channel\":%u,\"source\":\"%s\","
            "\"confidence\":%.3f,\"frequency_hz\":%.2f,"
            "\"timing_uncertainty_ms\":%u,"
            "\"target_midi\":%u,\"target_start\":%.3f,"
            "\"target_duration\":%.3f,\"played_start\":%.3f,"
            "\"aligned_played_start\":%.3f,"
            "\"played_duration\":%.3f,\"time_error\":%.3f,"
            "\"pitch_error\":%d,\"duration_error\":%.3f,"
            "\"result\":\"%s\"}",
            (unsigned)detail_count, step->target_index + 1,
            step->played_index + 1, target->midi, played->midi,
            target->start_ms, played->start_ms, aligned_start,
            raw_timing_error_ms, effective_timing_error_ms,
            note_rhythm_score, target->duration_ms, played->duration_ms,
            played->duration_reliable ? "true" : "false",
            (int)played->midi - (int)target->midi,
            played->velocity, played->channel, result_source,
            performance_note_confidence(played), played->frequency_hz,
            (unsigned)played->timing_uncertainty_ms,
            target->midi, (double)target->start_ms / 1000.0,
            (double)target->duration_ms / 1000.0,
            (double)played->start_ms / 1000.0,
            (double)aligned_start / 1000.0,
            (double)played->duration_ms / 1000.0,
            (double)effective_timing_error_ms / 1000.0,
            (int)played->midi - (int)target->midi,
            metrics.duration_error_ratio, result);
    }

    heap_caps_free(steps);
    heap_caps_free(reliable_notes);
    heap_caps_free(uncertain_used);
    if (details.failed) {
        heap_caps_free(details.data);
        set_error(error, error_length, "score_result_details_too_large");
        return ESP_ERR_NO_MEM;
    }

    const size_t pitch_denominator =
        correct_count + substitution_count;
    float pitch_score = 0.0f;
    if (pitch_denominator > 0) {
        const float extra_penalty = 15.0f *
            (float)extra_count /
            (float)(pitch_denominator + extra_count);
        pitch_score = clamp_float(
            pitch_credit_sum / (float)pitch_denominator - extra_penalty,
            0.0f, 100.0f);
    }
    const bool rhythm_evaluable = rhythm_count > 0;
    const float rhythm_score = rhythm_evaluable
                                   ? rhythm_sum / (float)rhythm_count
                                   : 0.0f;
    const float retry_penalty =
        clamp_float((float)retry_count * 3.0f, 0.0f, 12.0f);
    continuity_penalty = clamp_float(continuity_penalty, 0.0f, 20.0f);
    float fluency_score = clamp_float(
        100.0f - retry_penalty - continuity_penalty, 0.0f, 100.0f);
    float complete_score = 100.0f *
        (float)attempted_end_ms / (float)score_end_ms;
    complete_score = clamp_float(complete_score, 0.0f, 100.0f);

    const bool no_reliable_attempt = reliable_count == 0;
    float total_score = 0.0f;
    if (!no_reliable_attempt) {
        if (rhythm_evaluable) {
            total_score = 0.55f * pitch_score +
                          0.25f * rhythm_score +
                          0.10f * fluency_score +
                          0.10f * complete_score;
        } else {
            total_score = (0.55f * pitch_score +
                           0.10f * fluency_score +
                           0.10f * complete_score) / 0.75f;
        }
        total_score = clamp_float(total_score, 0.0f, 100.0f);
    } else {
        pitch_score = 0.0f;
        fluency_score = 0.0f;
        complete_score = 0.0f;
    }

    size_t observed_count = performance->observed_note_count;
    size_t input_uncertain_count = performance->uncertain_note_count;
    float confidence_sum = performance->confidence_sum;
    if (observed_count == 0 && performance->count > 0) {
        observed_count = performance->count;
        input_uncertain_count = derived_uncertain_count;
        confidence_sum = derived_confidence_sum;
    }
    const float average_confidence = !is_audio
        ? 1.0f
        : (observed_count > 0
               ? clamp_float(confidence_sum / (float)observed_count,
                             0.0f, 1.0f)
               : 0.0f);
    const float uncertain_ratio = observed_count > 0
        ? (float)input_uncertain_count / (float)observed_count
        : 0.0f;
    const float input_confidence = !is_audio
        ? 1.0f
        : clamp_float(average_confidence *
                          (1.0f - 0.50f * uncertain_ratio) -
                          0.05f * (float)performance->input_error_count,
                      0.0f, 1.0f);
    const float alignment_confidence = attempted_count > 0
        ? clamp_float((float)(correct_count + substitution_count) /
                          (float)(attempted_count + extra_count),
                      0.0f, 1.0f)
        : 0.0f;
    const float result_confidence =
        fminf(input_confidence, alignment_confidence);
    const bool scorable = !is_audio ||
        (reliable_count > 0 && input_confidence >= 0.45f &&
         uncertain_ratio <= 0.60f &&
         performance->input_error_count <= 12U);
    const bool reference_only = is_audio && scorable &&
        (input_confidence < 0.75f || uncertain_ratio > 0.20f ||
         performance->input_drop_count > 0U ||
         !performance->input_stream_healthy ||
         performance->input_error_count > 0U);
    const bool official_score = scorable && !reference_only;
    const float reported_total_score = scorable ? total_score : 0.0f;
    const char *score_status = !scorable
        ? "unscorable"
        : (reference_only ? "reference" : "official");
    const char *level = scorable ? score_level(total_score) : "无法评分";

    json_builder_t header = {0};
    json_append(&header,
                "{\"ok\":true,\"ready\":true,\"state\":\"ready\",\"title\":");
    json_append_escaped(&header, score->title);
    json_appendf(
        &header,
        ",\"input_source\":\"%s\",\"profile\":\"beginner_mono_v2\","
        "\"scoring_version\":\"2.1.0\",\"score_status\":\"%s\","
        "\"scorable\":%s,\"official_score\":%s,\"level\":",
        input_source, score_status,
        scorable ? "true" : "false",
        official_score ? "true" : "false");
    json_append_escaped(&header, level);
    json_appendf(
        &header,
        ",\"target_count\":%u,\"attempted_count\":%u,"
        "\"played_count\":%u,\"reliable_played_count\":%u,"
        "\"matched_count\":%u,\"correct_count\":%u,"
        "\"wrong_count\":%u,\"missing_count\":%u,"
        "\"extra_count\":%u,\"retry_count\":%u,"
        "\"uncertain_count\":%u,\"uncertain_target_count\":%u,"
        "\"start_anchor_target_index\":%d,"
        "\"start_anchor_played_index\":%d,"
        "\"leading_missing_count\":%u,\"leading_extra_count\":%u,"
        "\"alignment_origin_locked\":%s,\"tempo_sample_count\":%u,"
        "\"pitch_score\":%.1f,\"rhythm_score\":%.1f,"
        "\"rhythm_evaluable\":%s,\"fluency_score\":%.1f,"
        "\"complete_score\":%.1f,\"total_score\":%.1f,"
        "\"provisional_total_score\":%.1f,"
        "\"input_confidence\":%.3f,"
        "\"alignment_confidence\":%.3f,"
        "\"result_confidence\":%.3f,"
        "\"uncertain_ratio\":%.3f,"
        "\"input_drop_count\":%u,\"input_error_count\":%u,"
        "\"input_stream_healthy\":%s,"
        "\"tempo_scale\":%.4f,\"start_offset\":%.4f,"
        "\"start_offset_ms\":%.0f,"
        "\"alignment_method\":\"hard_origin_start_locked_edit_alignment\","
        "\"rhythm_method\":\"local_tempo_beat_normalized\","
        "\"duration_scored\":false,\"velocity_scored\":false,"
        "\"octave_correction\":false,\"details\":[",
        (unsigned)score->note_count, (unsigned)attempted_count,
        (unsigned)performance->count, (unsigned)reliable_count,
        (unsigned)(correct_count + substitution_count),
        (unsigned)correct_count, (unsigned)substitution_count,
        (unsigned)missing_count, (unsigned)extra_count,
        (unsigned)retry_count,
        (unsigned)input_uncertain_count,
        (unsigned)uncertain_target_count,
        start_anchor.valid ? (int)start_anchor.target_index + 1 : -1,
        start_anchor.valid ? (int)start_anchor.played_index + 1 : -1,
        start_anchor.valid ? (unsigned)start_anchor.target_index : 0U,
        start_anchor.valid ? (unsigned)start_anchor.played_index : 0U,
        start_anchor.valid ? "true" : "false",
        (unsigned)tempo_sample_count,
        pitch_score, rhythm_score,
        rhythm_evaluable ? "true" : "false", fluency_score,
        complete_score, reported_total_score, total_score,
        input_confidence, alignment_confidence, result_confidence,
        uncertain_ratio, (unsigned)performance->input_drop_count,
        (unsigned)performance->input_error_count,
        performance->input_stream_healthy ? "true" : "false",
        tempo_scale, start_offset_ms / 1000.0f,
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
