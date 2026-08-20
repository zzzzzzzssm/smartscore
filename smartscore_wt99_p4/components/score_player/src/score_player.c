#include "score_player.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "piano_sample_bank.h"
#include "speaker_service.h"

#define SCORE_PLAYER_SAMPLE_RATE_HZ 24000U
#define SCORE_PLAYER_CHUNK_SAMPLES 512U
#define SCORE_PLAYER_MAX_NOTES 512U
#define SCORE_PLAYER_MAX_VOICES 12U
#define SCORE_PLAYER_QUEUE_LENGTH 6U
#define SCORE_PLAYER_TASK_STACK_BYTES 8192U
#define SCORE_PLAYER_TASK_PRIORITY 8U
#define SCORE_PLAYER_STREAM_START_TIMEOUT_MS 1000U
#define SCORE_PLAYER_STREAM_RELEASE_BYTES (16U * 1024U)
#define SCORE_PLAYER_RELEASE_SAMPLES (SCORE_PLAYER_SAMPLE_RATE_HZ / 5U)
#define SCORE_PLAYER_WAVETABLE_BITS 10U
#define SCORE_PLAYER_WAVETABLE_SIZE (1U << SCORE_PLAYER_WAVETABLE_BITS)
#define SCORE_PLAYER_PHASE_SCALE 4294967296.0
#define SCORE_PLAYER_SAMPLE_POSITION_SCALE 65536.0
#define SCORE_PLAYER_RELEASE_COEFFICIENT 0.99825f
#define SCORE_PLAYER_SAMPLE_HOLD_COEFFICIENT 0.999995f

typedef struct {
    uint8_t midi;
    uint8_t velocity;
    uint8_t staff;
    uint8_t voice;
    uint64_t start_sample;
    uint64_t end_sample;
} score_player_note_t;

typedef struct {
    score_player_note_t *notes;
    size_t note_count;
    uint64_t total_samples;
} score_player_timeline_t;

typedef struct {
    bool active;
    bool releasing;
    uint32_t phase;
    uint32_t phase_step;
    uint32_t sample_position_q16;
    uint32_t sample_step_q16;
    uint64_t end_sample;
    float envelope;
    float velocity_gain;
    float frequency_hz;
    const piano_sample_zone_t *sample_zone;
} score_player_voice_t;

typedef enum {
    PLAYER_COMMAND_START = 0,
    PLAYER_COMMAND_PAUSE,
    PLAYER_COMMAND_RESUME,
    PLAYER_COMMAND_STOP,
} player_command_type_t;

typedef struct {
    player_command_type_t type;
    uint32_t generation;
    uint16_t bpm;
    char *json;
} player_command_t;

typedef struct {
    score_player_timeline_t timeline;
    piano_sample_bank_t *sample_bank;
    score_player_voice_t voices[SCORE_PLAYER_MAX_VOICES];
    uint32_t generation;
    uint32_t stream_generation;
    uint32_t previous_stream_generation;
    int64_t stream_start_us;
    uint64_t generated_sample;
    size_t next_note;
    size_t pending_offset;
    size_t pending_count;
    int16_t pending_pcm[SCORE_PLAYER_CHUNK_SAMPLES];
    bool waiting_for_stream;
    bool stream_released;
    bool finish_sent;
    bool paused;
    int last_first_note;
    int last_last_note;
    uint32_t last_position_ms;
} player_context_t;

static const char *TAG = "SCORE_PLAYER";
static QueueHandle_t s_command_queue;
static SemaphoreHandle_t s_status_lock;
static TaskHandle_t s_task;
static score_player_status_t s_status;
static score_player_status_cb_t s_status_callback;
static void *s_status_user_data;
static uint32_t s_next_generation;
static float s_sine_table[SCORE_PLAYER_WAVETABLE_SIZE];

static void *allocate_memory(size_t bytes)
{
    void *memory = heap_caps_malloc(bytes,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory ? memory : malloc(bytes);
}

static char *duplicate_json(const char *json)
{
    const size_t length = strlen(json);
    if (length == SIZE_MAX) return NULL;
    char *copy = allocate_memory(length + 1U);
    if (!copy) return NULL;
    memcpy(copy, json, length + 1U);
    return copy;
}

static int compare_notes(const void *left, const void *right)
{
    const score_player_note_t *a = left;
    const score_player_note_t *b = right;
    if (a->start_sample != b->start_sample) {
        return a->start_sample < b->start_sample ? -1 : 1;
    }
    if (a->midi != b->midi) return a->midi < b->midi ? -1 : 1;
    if (a->staff != b->staff) return a->staff < b->staff ? -1 : 1;
    if (a->voice != b->voice) return a->voice < b->voice ? -1 : 1;
    return 0;
}

static bool number_is_finite(const cJSON *item)
{
    return cJSON_IsNumber(item) && isfinite(item->valuedouble);
}

static uint64_t ticks_to_samples(double ticks, uint16_t bpm,
                                 uint32_t ticks_per_quarter)
{
    if (ticks <= 0.0) return 0;
    const double denominator = (double)bpm * ticks_per_quarter;
    const double samples = ticks * 60.0 * SCORE_PLAYER_SAMPLE_RATE_HZ /
                           denominator;
    if (samples >= (double)UINT64_MAX) return UINT64_MAX;
    return (uint64_t)(samples + 0.5);
}

static esp_err_t build_timeline(const char *json, uint16_t preview_bpm,
                                score_player_timeline_t *timeline)
{
    memset(timeline, 0, sizeof(*timeline));
    cJSON *root = cJSON_Parse(json);
    if (!root) return ESP_ERR_INVALID_ARG;

    cJSON *notes = cJSON_GetObjectItemCaseSensitive(root, "notes");
    if (!cJSON_IsArray(notes) || cJSON_GetArraySize(notes) <= 0) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *bpm_item = cJSON_GetObjectItemCaseSensitive(root, "bpm");
    cJSON *tpq_item = cJSON_GetObjectItemCaseSensitive(
        root, "ticks_per_quarter");
    uint16_t source_bpm = number_is_finite(bpm_item) &&
                                  bpm_item->valueint > 0
                              ? (uint16_t)bpm_item->valueint
                              : 120U;
    const uint16_t target_bpm = preview_bpm > 0U ? preview_bpm : source_bpm;
    const uint32_t tpq = number_is_finite(tpq_item) &&
                                 tpq_item->valueint > 0
                             ? (uint32_t)tpq_item->valueint
                             : 480U;
    if (target_bpm < 20U || target_bpm > 400U || tpq > 15360U) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    size_t capacity = (size_t)cJSON_GetArraySize(notes);
    if (capacity > SCORE_PLAYER_MAX_NOTES) capacity = SCORE_PLAYER_MAX_NOTES;
    score_player_note_t *parsed = allocate_memory(
        capacity * sizeof(*parsed));
    if (!parsed) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, notes) {
        if (count >= capacity) break;
        cJSON *midi_item = cJSON_GetObjectItemCaseSensitive(item, "midi");
        cJSON *start_item = cJSON_GetObjectItemCaseSensitive(item, "start");
        cJSON *duration_item = cJSON_GetObjectItemCaseSensitive(
            item, "duration");
        cJSON *start_tick_item = cJSON_GetObjectItemCaseSensitive(
            item, "start_tick");
        cJSON *duration_tick_item = cJSON_GetObjectItemCaseSensitive(
            item, "duration_ticks");
        if (!number_is_finite(midi_item) || midi_item->valueint < 0 ||
            midi_item->valueint > 127) {
            continue;
        }

        double start_ticks = -1.0;
        double duration_ticks = -1.0;
        if (number_is_finite(start_tick_item) &&
            start_tick_item->valuedouble >= 0.0) {
            start_ticks = start_tick_item->valuedouble;
        } else if (number_is_finite(start_item) &&
                   start_item->valuedouble >= 0.0) {
            start_ticks = start_item->valuedouble *
                          ((double)source_bpm / 60.0) * tpq;
        }
        if (number_is_finite(duration_tick_item) &&
            duration_tick_item->valuedouble > 0.0) {
            duration_ticks = duration_tick_item->valuedouble;
        } else if (number_is_finite(duration_item) &&
                   duration_item->valuedouble > 0.0) {
            duration_ticks = duration_item->valuedouble *
                             ((double)source_bpm / 60.0) * tpq;
        }
        if (start_ticks < 0.0 || duration_ticks <= 0.0) continue;

        const uint64_t start_sample = ticks_to_samples(
            start_ticks, target_bpm, tpq);
        uint64_t duration_samples = ticks_to_samples(
            duration_ticks, target_bpm, tpq);
        if (duration_samples == 0U) duration_samples = 1U;
        if (start_sample > UINT64_MAX - duration_samples) continue;

        cJSON *velocity_item = cJSON_GetObjectItemCaseSensitive(
            item, "velocity");
        cJSON *staff_item = cJSON_GetObjectItemCaseSensitive(item, "staff");
        cJSON *voice_item = cJSON_GetObjectItemCaseSensitive(item, "voice");
        int velocity = number_is_finite(velocity_item)
                           ? velocity_item->valueint
                           : 80;
        if (velocity < 1) velocity = 1;
        if (velocity > 127) velocity = 127;
        parsed[count] = (score_player_note_t) {
            .midi = (uint8_t)midi_item->valueint,
            .velocity = (uint8_t)velocity,
            .staff = number_is_finite(staff_item) && staff_item->valueint == 2
                         ? 2U : 1U,
            .voice = number_is_finite(voice_item) && voice_item->valueint > 0
                         ? (uint8_t)voice_item->valueint : 1U,
            .start_sample = start_sample,
            .end_sample = start_sample + duration_samples,
        };
        count++;
    }
    cJSON_Delete(root);
    if (count == 0U) {
        free(parsed);
        return ESP_ERR_INVALID_ARG;
    }
    qsort(parsed, count, sizeof(*parsed), compare_notes);
    uint64_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (parsed[i].end_sample > total) total = parsed[i].end_sample;
    }
    if (total > UINT64_MAX - SCORE_PLAYER_RELEASE_SAMPLES) {
        free(parsed);
        return ESP_ERR_INVALID_SIZE;
    }
    timeline->notes = parsed;
    timeline->note_count = count;
    timeline->total_samples = total + SCORE_PLAYER_RELEASE_SAMPLES;
    return ESP_OK;
}

static void release_timeline(score_player_timeline_t *timeline)
{
    free(timeline->notes);
    memset(timeline, 0, sizeof(*timeline));
}

static score_player_status_t status_snapshot(void)
{
    score_player_status_t status;
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    status = s_status;
    xSemaphoreGive(s_status_lock);
    return status;
}

static void publish_status(uint32_t generation, score_player_state_t state,
                           uint32_t position_ms, int first_note,
                           int last_note, esp_err_t error,
                           const char *message)
{
    score_player_status_cb_t callback = NULL;
    void *user_data = NULL;
    score_player_status_t snapshot;
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    if (s_status.generation != generation) {
        xSemaphoreGive(s_status_lock);
        return;
    }
    s_status.state = state;
    s_status.position_ms = position_ms;
    s_status.first_note_index = first_note;
    s_status.last_note_index = last_note;
    s_status.last_error = error;
    snprintf(s_status.message, sizeof(s_status.message), "%s",
             message ? message : "");
    snapshot = s_status;
    callback = s_status_callback;
    user_data = s_status_user_data;
    xSemaphoreGive(s_status_lock);
    if (callback) callback(&snapshot, user_data);
}

static uint32_t next_generation(void)
{
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    if (++s_next_generation == 0U) ++s_next_generation;
    s_status.generation = s_next_generation;
    s_status.state = SCORE_PLAYER_PREPARING;
    s_status.position_ms = 0;
    s_status.first_note_index = 0;
    s_status.last_note_index = 0;
    s_status.last_error = ESP_OK;
    s_status.message[0] = '\0';
    const uint32_t generation = s_next_generation;
    xSemaphoreGive(s_status_lock);
    return generation;
}

static bool generation_is_current(uint32_t generation)
{
    xSemaphoreTake(s_status_lock, portMAX_DELAY);
    const bool current = s_status.generation == generation;
    xSemaphoreGive(s_status_lock);
    return current;
}

static void reset_context(player_context_t *context, bool abort_stream)
{
    if (abort_stream && context->stream_generation != 0U) {
        speaker_stream_metrics_t metrics;
        speaker_service_stream_get_metrics(&metrics);
        if (metrics.active &&
            metrics.generation == context->stream_generation) {
            (void)speaker_service_stream_abort();
        }
    }
    piano_sample_bank_free(context->sample_bank);
    release_timeline(&context->timeline);
    memset(context, 0, sizeof(*context));
    context->last_first_note = -1;
    context->last_last_note = -1;
}

static float midi_frequency(uint8_t midi)
{
    return 440.0f * powf(2.0f, ((float)midi - 69.0f) / 12.0f);
}

static float wavetable_sample(uint32_t phase, unsigned harmonic)
{
    const uint32_t harmonic_phase = phase * harmonic;
    const uint32_t index = harmonic_phase >>
                           (32U - SCORE_PLAYER_WAVETABLE_BITS);
    return s_sine_table[index & (SCORE_PLAYER_WAVETABLE_SIZE - 1U)];
}

static score_player_voice_t *voice_for_note(player_context_t *context)
{
    score_player_voice_t *quietest = &context->voices[0];
    for (size_t i = 0; i < SCORE_PLAYER_MAX_VOICES; ++i) {
        score_player_voice_t *voice = &context->voices[i];
        if (!voice->active) return voice;
        if (voice->envelope < quietest->envelope) quietest = voice;
    }
    return quietest;
}

static void start_voice(player_context_t *context,
                        const score_player_note_t *note)
{
    score_player_voice_t *voice = voice_for_note(context);
    const float frequency = midi_frequency(note->midi);
    if (frequency >= SCORE_PLAYER_SAMPLE_RATE_HZ * 0.49f) {
        voice->active = false;
        return;
    }
    const piano_sample_zone_t *sample_zone =
        piano_sample_bank_select(context->sample_bank,
                                 note->midi, note->velocity);
    uint32_t sample_step_q16 = 0U;
    float velocity_gain = (float)note->velocity / 127.0f;
    if (sample_zone) {
        const double ratio = pow(2.0,
                                 ((double)note->midi -
                                  sample_zone->root_midi) / 12.0);
        sample_step_q16 = (uint32_t)(
            ratio * SCORE_PLAYER_SAMPLE_POSITION_SCALE + 0.5);
        if (sample_step_q16 == 0U) sample_step_q16 = 1U;
        velocity_gain = 0.65f + 0.50f * velocity_gain;
    }
    *voice = (score_player_voice_t) {
        .active = true,
        .phase = 0,
        .phase_step = (uint32_t)(frequency * SCORE_PLAYER_PHASE_SCALE /
                                 SCORE_PLAYER_SAMPLE_RATE_HZ),
        .sample_position_q16 = 0U,
        .sample_step_q16 = sample_step_q16,
        .end_sample = note->end_sample,
        .envelope = sample_zone ? 1.0f : 0.0f,
        .velocity_gain = velocity_gain,
        .frequency_hz = frequency,
        .sample_zone = sample_zone,
    };
}

static float render_sample_voice(score_player_voice_t *voice)
{
    const piano_sample_zone_t *zone = voice->sample_zone;
    if (!zone || zone->loop_start >= zone->loop_end ||
        zone->loop_end >= zone->sample_count) {
        voice->active = false;
        return 0.0f;
    }

    const uint32_t loop_start_q16 = zone->loop_start << 16U;
    const uint32_t loop_end_q16 = zone->loop_end << 16U;
    const uint32_t loop_length_q16 = loop_end_q16 - loop_start_q16;
    while (voice->sample_position_q16 >= loop_end_q16) {
        voice->sample_position_q16 = loop_start_q16 +
            (voice->sample_position_q16 - loop_end_q16) % loop_length_q16;
    }

    const uint32_t position = voice->sample_position_q16 >> 16U;
    const uint32_t fraction = voice->sample_position_q16 & UINT32_C(0xFFFF);
    const int32_t first = zone->samples[position];
    const int32_t second = zone->samples[position + 1U];
    const float wave = (float)(first +
        (int32_t)(((int64_t)(second - first) * fraction) >> 16U)) /
        32768.0f;
    voice->sample_position_q16 += voice->sample_step_q16;
    if (!voice->releasing) {
        voice->envelope *= SCORE_PLAYER_SAMPLE_HOLD_COEFFICIENT;
    }
    return wave * zone->gain * voice->envelope * voice->velocity_gain;
}

static float render_synth_voice(score_player_voice_t *voice)
{
    if (!voice->releasing) {
        if (voice->envelope < 1.0f) {
            voice->envelope += 1.0f / 120.0f;
            if (voice->envelope > 1.0f) voice->envelope = 1.0f;
        } else {
            voice->envelope *= 0.99992f;
        }
    }

    float wave = wavetable_sample(voice->phase, 1U);
    if (voice->frequency_hz * 2.0f < SCORE_PLAYER_SAMPLE_RATE_HZ * 0.49f)
        wave += 0.42f * wavetable_sample(voice->phase, 2U);
    if (voice->frequency_hz * 3.0f < SCORE_PLAYER_SAMPLE_RATE_HZ * 0.49f)
        wave += 0.18f * wavetable_sample(voice->phase, 3U);
    if (voice->frequency_hz * 4.0f < SCORE_PLAYER_SAMPLE_RATE_HZ * 0.49f)
        wave += 0.07f * wavetable_sample(voice->phase, 4U);
    voice->phase += voice->phase_step;
    return wave * (1.0f / 1.67f) * voice->envelope *
           voice->velocity_gain;
}

static float render_voice(score_player_voice_t *voice, uint64_t sample)
{
    if (!voice->active) return 0.0f;
    if (sample >= voice->end_sample) voice->releasing = true;
    if (voice->releasing) {
        voice->envelope *= SCORE_PLAYER_RELEASE_COEFFICIENT;
    }
    if (voice->envelope < 0.0005f) {
        voice->active = false;
        return 0.0f;
    }
    return voice->sample_zone ? render_sample_voice(voice)
                              : render_synth_voice(voice);
}

static size_t render_chunk(player_context_t *context)
{
    const uint64_t remaining = context->timeline.total_samples -
                               context->generated_sample;
    const size_t count = remaining < SCORE_PLAYER_CHUNK_SAMPLES
                             ? (size_t)remaining
                             : SCORE_PLAYER_CHUNK_SAMPLES;
    for (size_t out = 0; out < count; ++out) {
        const uint64_t sample = context->generated_sample + out;
        while (context->next_note < context->timeline.note_count &&
               context->timeline.notes[context->next_note].start_sample <=
                   sample) {
            start_voice(context,
                        &context->timeline.notes[context->next_note++]);
        }
        float mixed = 0.0f;
        for (size_t i = 0; i < SCORE_PLAYER_MAX_VOICES; ++i) {
            if (!context->voices[i].active) continue;
            mixed += render_voice(&context->voices[i], sample);
        }
        /* Fixed headroom avoids audible gain pumping when chord voice counts
         * change. A smooth knee then contains unusually dense passages. */
        mixed *= 0.58f;
        const float magnitude = fabsf(mixed);
        if (magnitude > 0.78f) {
            const float excess = magnitude - 0.78f;
            const float compressed = 0.78f + excess / (1.0f + 5.0f * excess);
            mixed = mixed < 0.0f ? -compressed : compressed;
        }
        if (mixed > 0.98f) mixed = 0.98f;
        if (mixed < -0.98f) mixed = -0.98f;
        context->pending_pcm[out] = (int16_t)(mixed * 32767.0f);
    }
    context->generated_sample += count;
    context->pending_offset = 0;
    context->pending_count = count;
    return count;
}

static bool sample_bank_load_cancelled(void *user_data)
{
    const player_context_t *context = user_data;
    return !context || !generation_is_current(context->generation);
}

static void try_load_sample_bank(player_context_t *context)
{
    if (!context || !context->timeline.notes ||
        context->timeline.note_count == 0U) {
        return;
    }
    piano_sample_request_t *requests = calloc(
        context->timeline.note_count, sizeof(*requests));
    if (!requests) {
        ESP_LOGW(TAG, "sample request allocation failed; using synth");
        return;
    }
    for (size_t index = 0; index < context->timeline.note_count; ++index) {
        requests[index].midi = context->timeline.notes[index].midi;
        requests[index].velocity = context->timeline.notes[index].velocity;
    }

    publish_status(context->generation, SCORE_PLAYER_PREPARING,
                   0, 0, 0, ESP_OK, "正在加载钢琴音色");
    char bank_error[96] = {0};
    const esp_err_t error = piano_sample_bank_load(
        PIANO_SAMPLE_BANK_PATH, requests, context->timeline.note_count,
        sample_bank_load_cancelled, context,
        &context->sample_bank, bank_error, sizeof(bank_error));
    free(requests);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "piano sample bank unavailable (%s): %s",
                 esp_err_to_name(error), bank_error);
        publish_status(context->generation, SCORE_PLAYER_PREPARING,
                       0, 0, 0, ESP_OK,
                       "钢琴音色不可用，使用基础音色");
        return;
    }
    ESP_LOGI(TAG, "loaded %u piano zones (%u bytes PSRAM)",
             (unsigned)piano_sample_bank_zone_count(context->sample_bank),
             (unsigned)piano_sample_bank_memory_bytes(context->sample_bank));
}

static void current_note_group(const player_context_t *context,
                               uint64_t played_sample,
                               int *first_note, int *last_note)
{
    *first_note = 0;
    *last_note = 0;
    if (!context->timeline.notes || context->timeline.note_count == 0U) return;
    size_t group_first = 0;
    bool found = false;
    for (size_t i = 0; i < context->timeline.note_count;) {
        size_t end = i + 1U;
        uint64_t group_end = context->timeline.notes[i].end_sample;
        while (end < context->timeline.note_count &&
               context->timeline.notes[end].start_sample ==
                   context->timeline.notes[i].start_sample) {
            if (context->timeline.notes[end].end_sample > group_end)
                group_end = context->timeline.notes[end].end_sample;
            end++;
        }
        if (context->timeline.notes[i].start_sample > played_sample) break;
        group_first = i;
        found = played_sample < group_end;
        if (end < context->timeline.note_count &&
            context->timeline.notes[end].start_sample <= played_sample) {
            i = end;
            continue;
        }
        if (found) {
            *first_note = (int)group_first + 1;
            *last_note = (int)end;
        }
        return;
    }
}

static void publish_progress(player_context_t *context,
                             const speaker_stream_metrics_t *metrics)
{
    const uint64_t played_sample = metrics->played_bytes / sizeof(int16_t);
    const uint32_t position_ms = (uint32_t)(
        played_sample * 1000ULL / SCORE_PLAYER_SAMPLE_RATE_HZ);
    int first = 0;
    int last = 0;
    current_note_group(context, played_sample, &first, &last);
    if (first == context->last_first_note &&
        last == context->last_last_note &&
        position_ms - context->last_position_ms < 40U) {
        return;
    }
    context->last_first_note = first;
    context->last_last_note = last;
    context->last_position_ms = position_ms;
    publish_status(context->generation,
                   context->paused ? SCORE_PLAYER_PAUSED :
                   context->finish_sent ? SCORE_PLAYER_DRAINING :
                                          SCORE_PLAYER_PLAYING,
                   position_ms, first, last, ESP_OK, NULL);
}

static void handle_start_command(player_context_t *context,
                                 player_command_t *command)
{
    reset_context(context, true);
    context->generation = command->generation;
    publish_status(context->generation, SCORE_PLAYER_PREPARING, 0, 0, 0,
                   ESP_OK, "正在准备播放");
    const esp_err_t parse_error = build_timeline(
        command->json, command->bpm, &context->timeline);
    free(command->json);
    command->json = NULL;
    if (parse_error != ESP_OK) {
        publish_status(context->generation, SCORE_PLAYER_ERROR, 0, 0, 0,
                       parse_error, "乐谱没有可播放音符");
        reset_context(context, false);
        return;
    }
    try_load_sample_bank(context);
    if (!generation_is_current(context->generation)) {
        reset_context(context, false);
        return;
    }
    speaker_stream_metrics_t before;
    speaker_service_stream_get_metrics(&before);
    context->previous_stream_generation = before.generation;
    const esp_err_t stream_error = speaker_service_stream_start_held(
        SCORE_PLAYER_SAMPLE_RATE_HZ);
    if (stream_error != ESP_OK) {
        publish_status(context->generation, SCORE_PLAYER_ERROR, 0, 0, 0,
                       stream_error, "扬声器不可用");
        reset_context(context, false);
        return;
    }
    context->waiting_for_stream = true;
    context->stream_start_us = esp_timer_get_time();
}

static void handle_command(player_context_t *context,
                           player_command_t *command)
{
    if (command->type == PLAYER_COMMAND_START) {
        if (!generation_is_current(command->generation)) {
            free(command->json);
            return;
        }
        handle_start_command(context, command);
        return;
    }
    if (!generation_is_current(command->generation)) return;
    if (command->type == PLAYER_COMMAND_STOP) {
        reset_context(context, true);
        context->generation = command->generation;
        publish_status(command->generation, SCORE_PLAYER_STOPPED,
                       0, 0, 0, ESP_OK, NULL);
    } else if (command->type == PLAYER_COMMAND_PAUSE &&
               context->timeline.notes && !context->paused) {
        const esp_err_t error = speaker_service_stream_pause();
        if (error == ESP_OK) {
            context->paused = true;
            const score_player_status_t status = status_snapshot();
            publish_status(command->generation, SCORE_PLAYER_PAUSED,
                           status.position_ms, status.first_note_index,
                           status.last_note_index, ESP_OK, NULL);
        }
    } else if (command->type == PLAYER_COMMAND_RESUME &&
               context->timeline.notes && context->paused) {
        const esp_err_t error = speaker_service_stream_release();
        if (error == ESP_OK) {
            context->paused = false;
            const score_player_status_t status = status_snapshot();
            publish_status(command->generation, SCORE_PLAYER_PLAYING,
                           status.position_ms, status.first_note_index,
                           status.last_note_index, ESP_OK, NULL);
        }
    }
}

static void player_task(void *argument)
{
    (void)argument;
    for (uint32_t i = 0; i < SCORE_PLAYER_WAVETABLE_SIZE; ++i) {
        s_sine_table[i] = sinf(6.28318530718f * i /
                               SCORE_PLAYER_WAVETABLE_SIZE);
    }
    player_context_t context = {.last_first_note = -1,
                                .last_last_note = -1};
    player_command_t command;
    while (true) {
        const TickType_t wait = context.timeline.notes
                                    ? 0 : portMAX_DELAY;
        if (xQueueReceive(s_command_queue, &command, wait) == pdTRUE) {
            handle_command(&context, &command);
            continue;
        }
        if (!context.timeline.notes) continue;

        speaker_stream_metrics_t metrics;
        speaker_service_stream_get_metrics(&metrics);
        if (context.waiting_for_stream) {
            if (metrics.active &&
                metrics.generation != context.previous_stream_generation) {
                context.stream_generation = metrics.generation;
                context.waiting_for_stream = false;
            } else if ((uint64_t)(esp_timer_get_time() -
                                  context.stream_start_us) /
                           1000ULL > SCORE_PLAYER_STREAM_START_TIMEOUT_MS) {
                publish_status(context.generation, SCORE_PLAYER_ERROR,
                               0, 0, 0, ESP_ERR_TIMEOUT,
                               "扬声器启动超时");
                reset_context(&context, false);
            } else {
                /* CONFIG_FREERTOS_HZ=100: delays below 10 ms round to zero. */
                vTaskDelay(1);
            }
            continue;
        }
        if (!metrics.active ||
            metrics.generation != context.stream_generation) {
            const uint32_t generation = context.generation;
            reset_context(&context, false);
            context.generation = generation;
            publish_status(generation, SCORE_PLAYER_STOPPED,
                           0, 0, 0, ESP_OK, NULL);
            continue;
        }
        /* Keep the UI in PREPARING while the held stream is only being
         * filled. Advancing the guide from received PCM would make the first
         * note light up before the codec has actually started playing. */
        if (metrics.output_started || context.paused || context.finish_sent) {
            publish_progress(&context, &metrics);
        }
        if (context.paused) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (context.pending_offset < context.pending_count) {
            size_t accepted = 0;
            const esp_err_t error = speaker_service_stream_write(
                context.pending_pcm + context.pending_offset,
                context.pending_count - context.pending_offset, &accepted);
            context.pending_offset += accepted;
            if (error != ESP_OK && error != ESP_ERR_TIMEOUT) {
                publish_status(context.generation, SCORE_PLAYER_ERROR,
                               context.last_position_ms, 0, 0, error,
                               "音频输出失败");
                reset_context(&context, true);
            } else if (accepted == 0U) {
                /* Yield for one real RTOS tick when the PCM ring is full. */
                vTaskDelay(1);
            }
            continue;
        }
        if (!context.stream_released &&
            (metrics.received_bytes >= SCORE_PLAYER_STREAM_RELEASE_BYTES ||
             context.generated_sample >= context.timeline.total_samples)) {
            const esp_err_t error = speaker_service_stream_release();
            if (error != ESP_OK) {
                publish_status(context.generation, SCORE_PLAYER_ERROR,
                               0, 0, 0, error, "无法开始播放");
                reset_context(&context, true);
                continue;
            }
            context.stream_released = true;
        }
        if (context.generated_sample < context.timeline.total_samples) {
            (void)render_chunk(&context);
            continue;
        }
        if (!context.finish_sent) {
            const esp_err_t error = speaker_service_stream_finish();
            if (error != ESP_OK) {
                publish_status(context.generation, SCORE_PLAYER_ERROR,
                               context.last_position_ms, 0, 0, error,
                               "无法结束播放");
                reset_context(&context, true);
                continue;
            }
            context.finish_sent = true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t score_player_init(score_player_status_cb_t callback,
                            void *user_data)
{
    if (s_task) {
        s_status_callback = callback;
        s_status_user_data = user_data;
        return ESP_OK;
    }
    s_status_lock = xSemaphoreCreateMutex();
    s_command_queue = xQueueCreate(SCORE_PLAYER_QUEUE_LENGTH,
                                   sizeof(player_command_t));
    if (!s_status_lock || !s_command_queue) {
        if (s_status_lock) vSemaphoreDelete(s_status_lock);
        if (s_command_queue) vQueueDelete(s_command_queue);
        s_status_lock = NULL;
        s_command_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = SCORE_PLAYER_STOPPED;
    s_status_callback = callback;
    s_status_user_data = user_data;
    if (xTaskCreate(player_task, "score_player",
                    SCORE_PLAYER_TASK_STACK_BYTES, NULL,
                    SCORE_PLAYER_TASK_PRIORITY, &s_task) != pdPASS) {
        vSemaphoreDelete(s_status_lock);
        vQueueDelete(s_command_queue);
        s_status_lock = NULL;
        s_command_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "score player ready at %u Hz with %u voices",
             SCORE_PLAYER_SAMPLE_RATE_HZ, SCORE_PLAYER_MAX_VOICES);
    return ESP_OK;
}

esp_err_t score_player_start(const char *score_json, uint16_t preview_bpm)
{
    if (!s_task || !s_command_queue) return ESP_ERR_INVALID_STATE;
    if (!score_json || !score_json[0] || preview_bpm < 20U ||
        preview_bpm > 400U) {
        return ESP_ERR_INVALID_ARG;
    }
    char *copy = duplicate_json(score_json);
    if (!copy) return ESP_ERR_NO_MEM;
    player_command_t command = {
        .type = PLAYER_COMMAND_START,
        .generation = next_generation(),
        .bpm = preview_bpm,
        .json = copy,
    };
    if (xQueueSendToFront(s_command_queue, &command,
                          pdMS_TO_TICKS(50)) != pdTRUE) {
        free(copy);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t send_state_command(player_command_type_t type,
                                    bool new_generation)
{
    if (!s_task || !s_command_queue) return ESP_ERR_INVALID_STATE;
    uint32_t generation;
    if (new_generation) {
        generation = next_generation();
    } else {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
        generation = s_status.generation;
        xSemaphoreGive(s_status_lock);
    }
    player_command_t command = {.type = type, .generation = generation};
    return xQueueSendToFront(s_command_queue, &command,
                             pdMS_TO_TICKS(50)) == pdTRUE
               ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t score_player_pause(void)
{
    return send_state_command(PLAYER_COMMAND_PAUSE, false);
}

esp_err_t score_player_resume(void)
{
    return send_state_command(PLAYER_COMMAND_RESUME, false);
}

esp_err_t score_player_stop(void)
{
    return send_state_command(PLAYER_COMMAND_STOP, true);
}

void score_player_get_status(score_player_status_t *status)
{
    if (!status) return;
    if (!s_status_lock) {
        memset(status, 0, sizeof(*status));
        status->state = SCORE_PLAYER_STOPPED;
        return;
    }
    *status = status_snapshot();
}
