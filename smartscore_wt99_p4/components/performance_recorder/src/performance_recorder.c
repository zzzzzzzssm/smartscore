#include "performance_recorder.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    bool used;
    uint8_t midi;
    uint8_t velocity;
    uint8_t channel;
    uint64_t start_us;
} active_note_t;

typedef struct {
    SemaphoreHandle_t lock;
    performance_recorder_status_t status;
    performance_note_t *notes;
    uint64_t recording_start_us;
    uint64_t pause_started_us;
    uint64_t paused_total_us;
    active_note_t active[PERFORMANCE_RECORDER_MAX_ACTIVE_NOTES];
} recorder_t;

static const char *TAG = "PERF_RECORDER";
static recorder_t s_recorder;

static uint32_t elapsed_ms(uint64_t start_us, uint64_t end_us)
{
    if (end_us <= start_us) {
        return 0;
    }
    uint64_t value = (end_us - start_us) / 1000ULL;
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static uint64_t timeline_us_locked(uint64_t timestamp_us)
{
    uint64_t paused = s_recorder.paused_total_us;
    return timestamp_us > paused ? timestamp_us - paused : 0;
}

static void clear_active_locked(void)
{
    memset(s_recorder.active, 0, sizeof(s_recorder.active));
    s_recorder.status.active_count = 0;
}

static void free_notes_locked(void)
{
    if (s_recorder.notes != NULL) {
        heap_caps_free(s_recorder.notes);
        s_recorder.notes = NULL;
    }
    s_recorder.status.note_capacity = 0;
    s_recorder.status.buffer_in_psram = false;
}

static void set_error_locked(esp_err_t error,
                             const char *code,
                             const char *message)
{
    s_recorder.status.state = PERFORMANCE_RECORDER_ERROR;
    s_recorder.status.last_error = error;
    strlcpy(s_recorder.status.error,
            code != NULL ? code : esp_err_to_name(error),
            sizeof(s_recorder.status.error));
    strlcpy(s_recorder.status.message,
            message != NULL ? message : s_recorder.status.error,
            sizeof(s_recorder.status.message));
    clear_active_locked();
    free_notes_locked();
}

static bool append_note_locked(const active_note_t *active, uint64_t end_us)
{
    if (s_recorder.status.note_count >= s_recorder.status.note_capacity) {
        set_error_locked(ESP_ERR_NO_MEM,
                         "performance_note_capacity_exceeded",
                         "performance note capacity reached; recording stopped");
        return false;
    }
    performance_note_t *note =
        &s_recorder.notes[s_recorder.status.note_count++];
    note->midi = active->midi;
    note->start_ms = elapsed_ms(s_recorder.recording_start_us,
                                active->start_us);
    note->duration_ms = elapsed_ms(active->start_us, end_us);
    note->velocity = active->velocity;
    note->channel = active->channel;
    note->source = s_recorder.status.input_source;
    return true;
}

static int find_active_locked(uint8_t channel, uint8_t midi)
{
    for (size_t index = 0;
         index < PERFORMANCE_RECORDER_MAX_ACTIVE_NOTES; ++index) {
        if (s_recorder.active[index].used &&
            s_recorder.active[index].channel == channel &&
            s_recorder.active[index].midi == midi) {
            return (int)index;
        }
    }
    return -1;
}

static int find_free_active_locked(void)
{
    for (size_t index = 0;
         index < PERFORMANCE_RECORDER_MAX_ACTIVE_NOTES; ++index) {
        if (!s_recorder.active[index].used) {
            return (int)index;
        }
    }
    return -1;
}

static bool close_active_locked(size_t index, uint64_t timestamp_us)
{
    active_note_t active = s_recorder.active[index];
    s_recorder.active[index].used = false;
    if (s_recorder.status.active_count > 0) {
        --s_recorder.status.active_count;
    }
    return append_note_locked(&active, timestamp_us);
}

esp_err_t performance_recorder_init(void)
{
    if (s_recorder.lock != NULL) {
        return ESP_OK;
    }
    memset(&s_recorder, 0, sizeof(s_recorder));
    s_recorder.lock = xSemaphoreCreateMutex();
    if (s_recorder.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_recorder.status.state = PERFORMANCE_RECORDER_IDLE;
    s_recorder.status.last_error = ESP_OK;
    return ESP_OK;
}

esp_err_t performance_recorder_start(input_source_t source)
{
    if (s_recorder.lock == NULL ||
        (source != INPUT_SOURCE_USB_MIDI &&
         source != INPUT_SOURCE_AUDIO_S3)) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_recorder.lock, portMAX_DELAY);
    if (s_recorder.status.state == PERFORMANCE_RECORDER_RECORDING) {
        xSemaphoreGive(s_recorder.lock);
        return ESP_ERR_INVALID_STATE;
    }

    free_notes_locked();
    clear_active_locked();
    s_recorder.status.note_count = 0;
    s_recorder.pause_started_us = 0;
    s_recorder.paused_total_us = 0;
    s_recorder.status.input_source = source;
    s_recorder.status.last_error = ESP_OK;
    s_recorder.status.error[0] = '\0';
    s_recorder.status.message[0] = '\0';

    size_t bytes = PERFORMANCE_RECORDER_MAX_NOTES *
                   sizeof(performance_note_t);
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        s_recorder.notes = heap_caps_malloc(bytes,
                                            MALLOC_CAP_SPIRAM |
                                                MALLOC_CAP_8BIT);
        s_recorder.status.buffer_in_psram = s_recorder.notes != NULL;
    }
    if (s_recorder.notes == NULL) {
        s_recorder.notes = heap_caps_malloc(bytes,
                                            MALLOC_CAP_INTERNAL |
                                                MALLOC_CAP_8BIT);
        s_recorder.status.buffer_in_psram = false;
    }
    if (s_recorder.notes == NULL) {
        set_error_locked(ESP_ERR_NO_MEM,
                         "performance_buffer_allocation_failed",
                         "unable to allocate performance note buffer");
        xSemaphoreGive(s_recorder.lock);
        return ESP_ERR_NO_MEM;
    }

    s_recorder.status.note_capacity = PERFORMANCE_RECORDER_MAX_NOTES;
    s_recorder.recording_start_us = (uint64_t)esp_timer_get_time();
    s_recorder.status.state = PERFORMANCE_RECORDER_RECORDING;
    ESP_LOGI(TAG, "recording started: capacity=%u memory=%s",
             PERFORMANCE_RECORDER_MAX_NOTES,
             s_recorder.status.buffer_in_psram ? "PSRAM" : "internal");
    xSemaphoreGive(s_recorder.lock);
    return ESP_OK;
}

esp_err_t performance_recorder_pause(void)
{
    if (s_recorder.lock == NULL) return ESP_ERR_INVALID_STATE;

    uint64_t now_us = (uint64_t)esp_timer_get_time();
    xSemaphoreTake(s_recorder.lock, portMAX_DELAY);
    if (s_recorder.status.state != PERFORMANCE_RECORDER_RECORDING) {
        xSemaphoreGive(s_recorder.lock);
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t pause_timeline_us = timeline_us_locked(now_us);
    for (size_t index = 0;
         index < PERFORMANCE_RECORDER_MAX_ACTIVE_NOTES; ++index) {
        if (s_recorder.active[index].used &&
            !close_active_locked(index, pause_timeline_us)) {
            esp_err_t err = s_recorder.status.last_error;
            xSemaphoreGive(s_recorder.lock);
            return err;
        }
    }
    s_recorder.pause_started_us = now_us;
    s_recorder.status.state = PERFORMANCE_RECORDER_PAUSED;
    strlcpy(s_recorder.status.message, "performance paused",
            sizeof(s_recorder.status.message));
    xSemaphoreGive(s_recorder.lock);
    return ESP_OK;
}

esp_err_t performance_recorder_resume(void)
{
    if (s_recorder.lock == NULL) return ESP_ERR_INVALID_STATE;

    uint64_t now_us = (uint64_t)esp_timer_get_time();
    xSemaphoreTake(s_recorder.lock, portMAX_DELAY);
    if (s_recorder.status.state != PERFORMANCE_RECORDER_PAUSED ||
        s_recorder.pause_started_us == 0) {
        xSemaphoreGive(s_recorder.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (now_us > s_recorder.pause_started_us) {
        s_recorder.paused_total_us += now_us - s_recorder.pause_started_us;
    }
    s_recorder.pause_started_us = 0;
    s_recorder.status.state = PERFORMANCE_RECORDER_RECORDING;
    strlcpy(s_recorder.status.message, "recording note performance",
            sizeof(s_recorder.status.message));
    xSemaphoreGive(s_recorder.lock);
    return ESP_OK;
}

void performance_recorder_process_midi(bool note_on,
                                       uint8_t midi,
                                       uint8_t velocity,
                                       uint8_t channel,
                                       uint64_t timestamp_us)
{
    if (s_recorder.lock == NULL || midi > 127U || channel > 15U) {
        return;
    }
    xSemaphoreTake(s_recorder.lock, portMAX_DELAY);
    if (s_recorder.status.state != PERFORMANCE_RECORDER_RECORDING) {
        xSemaphoreGive(s_recorder.lock);
        return;
    }
    timestamp_us = timeline_us_locked(timestamp_us);

    int active_index = find_active_locked(channel, midi);
    if (note_on) {
        if (active_index >= 0) {
            if (!close_active_locked((size_t)active_index, timestamp_us)) {
                xSemaphoreGive(s_recorder.lock);
                return;
            }
        } else {
            active_index = find_free_active_locked();
        }
        if (active_index < 0) {
            set_error_locked(ESP_ERR_NO_MEM,
                             "active_note_capacity_exceeded",
                             "more than 64 simultaneous MIDI notes");
            xSemaphoreGive(s_recorder.lock);
            return;
        }
        s_recorder.active[active_index] = (active_note_t) {
            .used = true,
            .midi = midi,
            .velocity = velocity,
            .channel = channel,
            .start_us = timestamp_us,
        };
        ++s_recorder.status.active_count;
    } else if (active_index >= 0) {
        close_active_locked((size_t)active_index, timestamp_us);
    }
    xSemaphoreGive(s_recorder.lock);
}

esp_err_t performance_recorder_stop_and_take_snapshot(
    performance_snapshot_t *out_snapshot)
{
    if (s_recorder.lock == NULL || out_snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    uint64_t stop_us = (uint64_t)esp_timer_get_time();
    xSemaphoreTake(s_recorder.lock, portMAX_DELAY);
    bool paused = s_recorder.status.state == PERFORMANCE_RECORDER_PAUSED;
    if (s_recorder.status.state != PERFORMANCE_RECORDER_RECORDING && !paused) {
        esp_err_t err = s_recorder.status.last_error != ESP_OK
                            ? s_recorder.status.last_error
                            : ESP_ERR_INVALID_STATE;
        xSemaphoreGive(s_recorder.lock);
        return err;
    }
    uint64_t timeline_stop_us = paused
                                    ? timeline_us_locked(
                                          s_recorder.pause_started_us)
                                    : timeline_us_locked(stop_us);
    for (size_t index = 0;
         index < PERFORMANCE_RECORDER_MAX_ACTIVE_NOTES; ++index) {
        if (s_recorder.active[index].used &&
            !close_active_locked(index, timeline_stop_us)) {
            xSemaphoreGive(s_recorder.lock);
            return s_recorder.status.last_error;
        }
    }

    out_snapshot->notes = s_recorder.notes;
    out_snapshot->count = s_recorder.status.note_count;
    out_snapshot->duration_ms = elapsed_ms(s_recorder.recording_start_us,
                                            timeline_stop_us);
    out_snapshot->input_source = s_recorder.status.input_source;
    s_recorder.notes = NULL;
    s_recorder.status.note_capacity = 0;
    s_recorder.status.active_count = 0;
    s_recorder.status.state = PERFORMANCE_RECORDER_STOPPED;
    xSemaphoreGive(s_recorder.lock);
    return ESP_OK;
}

void performance_recorder_abort(esp_err_t error,
                                const char *code,
                                const char *message)
{
    if (s_recorder.lock == NULL) {
        return;
    }
    xSemaphoreTake(s_recorder.lock, portMAX_DELAY);
    if (s_recorder.status.state == PERFORMANCE_RECORDER_RECORDING ||
        s_recorder.status.state == PERFORMANCE_RECORDER_PAUSED) {
        set_error_locked(error, code, message);
        ESP_LOGE(TAG, "recording aborted: %s", s_recorder.status.error);
    }
    xSemaphoreGive(s_recorder.lock);
}

void performance_recorder_get_status(performance_recorder_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }
    memset(out_status, 0, sizeof(*out_status));
    if (s_recorder.lock == NULL) {
        out_status->state = PERFORMANCE_RECORDER_UNINITIALIZED;
        return;
    }
    xSemaphoreTake(s_recorder.lock, portMAX_DELAY);
    *out_status = s_recorder.status;
    xSemaphoreGive(s_recorder.lock);
}

void performance_snapshot_release(performance_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    heap_caps_free(snapshot->notes);
    memset(snapshot, 0, sizeof(*snapshot));
}
