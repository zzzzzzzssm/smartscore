#include "s3_bus.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "board_wt99_pins.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "s3_protocol.h"

#define S3_MUSIC_UART_RX_BUFFER_BYTES 2048
#define S3_MUSIC_UART_TX_BUFFER_BYTES 512
#define S3_MUSIC_LINE_MAX_BYTES       384
#define S3_MUSIC_EVENT_QUEUE_LENGTH   32
#define S3_MUSIC_RX_TASK_STACK_BYTES  6144
#define S3_MUSIC_RX_TASK_PRIORITY     8
#define S3_MUSIC_DISPATCH_STACK_BYTES 4096
#define S3_MUSIC_DISPATCH_PRIORITY    7
#define S3_MUSIC_POLL_STACK_BYTES     3072
#define S3_MUSIC_POLL_PRIORITY        6
#define S3_MUSIC_POLL_INTERVAL_MS     50
#define S3_MUSIC_ONLINE_TIMEOUT_MS    3000
#define S3_MUSIC_DIAG_INTERVAL_MS     5000
#define S3_MUSIC_COMMAND_MAX_BYTES    128

_Static_assert(S3_MUSIC_POLY_MAX_NOTES == S3_PROTOCOL_POLY_MAX_NOTES,
               "public and parser poly capacities must match");
_Static_assert(S3_MUSIC_POLY_NAME_MAX == S3_PROTOCOL_POLY_NAME_MAX,
               "public and parser poly name capacities must match");
_Static_assert(S3_MUSIC_NOTE_SET_MAX_NOTES ==
                   S3_PROTOCOL_NOTE_SET_MAX_NOTES,
               "public and parser note-set capacities must match");
_Static_assert(S3_MUSIC_DIAGNOSTIC_MAX_RAW ==
                   S3_PROTOCOL_DIAGNOSTIC_MAX_RAW,
               "public and parser diagnostic raw capacities must match");
_Static_assert(S3_MUSIC_DIAGNOSTIC_MAX_NOTES ==
                   S3_PROTOCOL_DIAGNOSTIC_MAX_NOTES,
               "public and parser diagnostic note capacities must match");
_Static_assert(S3_MUSIC_DIAGNOSTIC_REASON_MAX ==
                   S3_PROTOCOL_DIAGNOSTIC_REASON_MAX,
               "public and parser diagnostic reason capacities must match");

static const char *TAG = "S3_MUSIC";
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_event_queue;
static SemaphoreHandle_t s_tx_lock;
static SemaphoreHandle_t s_note_lock;
static TaskHandle_t s_rx_task;
static TaskHandle_t s_dispatch_task;
static TaskHandle_t s_poll_task;
static s3_music_event_handler_t s_handler;
static void *s_handler_context;
static s3_bus_status_t s_status;
static bool s_received_any;
static bool s_sequence_valid;
static bool s_note_stream_logged;
static bool s_poly_stream_logged;
static bool s_note_set_stream_logged;
static uint32_t s_requested_sid;
static s3_music_stream_profile_t s_requested_profile =
    S3_MUSIC_STREAM_PROFILE_PERFORMANCE;
static uint32_t s_ack_state_id;
static uint32_t s_note_state_sid;
static uint32_t s_last_note_state_id;
static uint8_t s_active_midis[S3_MUSIC_NOTE_SET_MAX_NOTES];
static uint8_t s_active_note_count;
static uint32_t s_note_on_ts[128];
static bool s_timeout_released;

static uint32_t local_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void increment_invalid(void)
{
    taskENTER_CRITICAL(&s_status_lock);
    ++s_status.invalid_frames;
    taskEXIT_CRITICAL(&s_status_lock);
}

static void increment_oversized(void)
{
    taskENTER_CRITICAL(&s_status_lock);
    ++s_status.oversized_lines;
    taskEXIT_CRITICAL(&s_status_lock);
}

static bool bus_initialized(void)
{
    taskENTER_CRITICAL(&s_status_lock);
    const bool initialized = s_status.initialized;
    taskEXIT_CRITICAL(&s_status_lock);
    return initialized;
}

static uint32_t sender_elapsed_ms(uint32_t now_ms, uint32_t start_ms)
{
    const uint32_t elapsed = now_ms - start_ms;
    return (int32_t)elapsed >= 0 ? elapsed : 0;
}

static esp_err_t send_command_line(const char *line, size_t length)
{
    if (line == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_status_lock);
    const bool initialized = s_status.initialized;
    taskEXIT_CRITICAL(&s_status_lock);
    if (!initialized || s_tx_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    const int written = uart_write_bytes(
        (uart_port_t)BOARD_WT99_MUSIC_UART_PORT, line, length);
    xSemaphoreGive(s_tx_lock);

    taskENTER_CRITICAL(&s_status_lock);
    ++s_status.tx_commands;
    if (written != (int)length) {
        ++s_status.tx_failures;
    }
    taskEXIT_CRITICAL(&s_status_lock);
    return written == (int)length ? ESP_OK : ESP_FAIL;
}

static int active_index(uint8_t midi)
{
    for (uint8_t index = 0; index < s_active_note_count; ++index) {
        if (s_active_midis[index] == midi) return index;
    }
    return -1;
}

static bool queue_event_batch(const s3_music_event_t *events, size_t count)
{
    if (count == 0) return true;
    if (s_event_queue == NULL ||
        uxQueueSpacesAvailable(s_event_queue) < count) {
        taskENTER_CRITICAL(&s_status_lock);
        s_status.dropped_events += (uint32_t)count;
        ++s_status.atomic_set_retries;
        taskEXIT_CRITICAL(&s_status_lock);
        return false;
    }
    for (size_t index = 0; index < count; ++index) {
        if (xQueueSend(s_event_queue, &events[index], 0) != pdTRUE) {
            taskENTER_CRITICAL(&s_status_lock);
            ++s_status.dropped_events;
            ++s_status.atomic_set_retries;
            taskEXIT_CRITICAL(&s_status_lock);
            return false;
        }
    }
    return true;
}

static bool release_active_notes(uint32_t sid, uint32_t sender_ts_ms,
                                 uint32_t seq)
{
    if (s_note_lock == NULL) return true;
    xSemaphoreTake(s_note_lock, portMAX_DELAY);
    s3_music_event_t events[S3_MUSIC_NOTE_SET_MAX_NOTES];
    for (uint8_t index = 0; index < s_active_note_count; ++index) {
        const uint8_t midi = s_active_midis[index];
        events[index] = (s3_music_event_t) {
            .type = S3_MUSIC_EVENT_NOTE_OFF,
            .seq = seq,
            .sid = sid,
            .sender_ts_ms = sender_ts_ms,
            .duration_ms = sender_elapsed_ms(sender_ts_ms,
                                             s_note_on_ts[midi]),
            .midi = midi,
            .velocity = 0,
            .has_duration = true,
        };
    }
    const bool queued = queue_event_batch(events, s_active_note_count);
    if (queued) {
        s_active_note_count = 0;
        memset(s_active_midis, 0, sizeof(s_active_midis));
        memset(s_note_on_ts, 0, sizeof(s_note_on_ts));
        taskENTER_CRITICAL(&s_status_lock);
        s_status.active_note_count = 0;
        taskEXIT_CRITICAL(&s_status_lock);
    }
    xSemaphoreGive(s_note_lock);
    return queued;
}

static esp_err_t send_poll(void)
{
    char command[S3_MUSIC_COMMAND_MAX_BYTES];
    taskENTER_CRITICAL(&s_status_lock);
    const uint32_t sid = s_requested_sid;
    const uint32_t ack_state_id = s_ack_state_id;
    taskEXIT_CRITICAL(&s_status_lock);
    const int length = snprintf(
        command, sizeof(command),
        "{\"cmd\":\"poll\",\"sid\":%" PRIu32
        ",\"ack_state_id\":%" PRIu32 "}\n",
        sid, ack_state_id);
    if (length < 0 || (size_t)length >= sizeof(command)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_err_t err = send_command_line(command, (size_t)length);
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&s_status_lock);
        ++s_status.polls_sent;
        taskEXIT_CRITICAL(&s_status_lock);
    }
    return err;
}

static void music_poll_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(10));
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t last_diag_ms = local_now_ms();
    while (true) {
        taskENTER_CRITICAL(&s_status_lock);
        const bool timed_out = s_received_any && !s_timeout_released &&
            (uint32_t)(local_now_ms() - s_status.last_rx_ms) >
                S3_MUSIC_ONLINE_TIMEOUT_MS;
        const uint32_t timeout_sid = s_status.last_sid;
        const uint32_t timeout_ts = s_status.last_sender_ts_ms +
                                    S3_MUSIC_ONLINE_TIMEOUT_MS;
        const uint32_t timeout_seq = s_status.last_seq;
        taskEXIT_CRITICAL(&s_status_lock);
        if (timed_out &&
            release_active_notes(timeout_sid, timeout_ts, timeout_seq)) {
            xSemaphoreTake(s_note_lock, portMAX_DELAY);
            s_note_state_sid = 0;
            s_last_note_state_id = 0;
            xSemaphoreGive(s_note_lock);
            taskENTER_CRITICAL(&s_status_lock);
            s_timeout_released = true;
            s_ack_state_id = 0;
            s_status.ack_state_id = 0;
            s_status.last_state_id = 0;
            s_status.degraded_mic = false;
            s_status.overflow = false;
            taskEXIT_CRITICAL(&s_status_lock);
            ESP_LOGW(TAG, "music link timeout: released all active S3 notes");
        }
        (void)send_poll();
        const uint32_t now_ms = local_now_ms();
        if ((uint32_t)(now_ms - last_diag_ms) >= S3_MUSIC_DIAG_INTERVAL_MS) {
            s3_bus_status_t diag;
            s3_bus_get_status(&diag);
            ESP_LOGI(TAG,
                     "v2 link diag: online=%d rx=%" PRIu32
                     " valid=%" PRIu32 " invalid=%" PRIu32
                     " sid=%" PRIu32 " state=%" PRIu32
                     " ack=%" PRIu32 " active=%u overflow=%d retries=%" PRIu32,
                     diag.online, diag.rx_bytes, diag.valid_frames,
                     diag.invalid_frames, diag.last_sid,
                     diag.last_state_id, diag.ack_state_id,
                     (unsigned)diag.active_note_count,
                     diag.overflow, diag.atomic_set_retries);
            last_diag_ms = now_ms;
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(S3_MUSIC_POLL_INTERVAL_MS));
    }
}

esp_err_t s3_bus_ping(void)
{
    static const char command[] = "{\"cmd\":\"ping\"}\n";
    return send_command_line(command, sizeof(command) - 1U);
}

static const char *profile_command_name(s3_music_stream_profile_t profile)
{
    switch (profile) {
    case S3_MUSIC_STREAM_PROFILE_STRICT:
        return "strict";
    case S3_MUSIC_STREAM_PROFILE_DEMO:
        return "demo";
    case S3_MUSIC_STREAM_PROFILE_PERFORMANCE:
        return "performance";
    default:
        return NULL;
    }
}

esp_err_t s3_bus_start_stream(uint32_t sid)
{
    return s3_bus_start_stream_with_profile(
        sid, S3_MUSIC_STREAM_PROFILE_PERFORMANCE);
}

esp_err_t s3_bus_start_stream_with_profile(
    uint32_t sid, s3_music_stream_profile_t profile)
{
    if (!bus_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *profile_name = profile_command_name(profile);
    if (profile_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_status_lock);
    const uint32_t previous_sid = s_status.last_sid;
    const uint32_t sender_ts_ms = s_status.last_sender_ts_ms;
    const uint32_t seq = s_status.last_seq;
    taskEXIT_CRITICAL(&s_status_lock);
    if (!release_active_notes(previous_sid, sender_ts_ms, seq)) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_note_lock, portMAX_DELAY);
    s_note_state_sid = sid;
    s_last_note_state_id = 0;
    xSemaphoreGive(s_note_lock);

    char command[S3_MUSIC_COMMAND_MAX_BYTES];
    const int length = snprintf(
        command, sizeof(command),
        "{\"cmd\":\"start\",\"sid\":%" PRIu32
        ",\"protocol\":2,\"profile\":\"%s\"}\n",
        sid, profile_name);
    if (length < 0 || (size_t)length >= sizeof(command)) {
        return ESP_ERR_INVALID_SIZE;
    }
    taskENTER_CRITICAL(&s_status_lock);
    s_status.stream_requested = true;
    s_requested_sid = sid;
    s_requested_profile = profile;
    s_ack_state_id = 0;
    s_status.ack_state_id = 0;
    s_status.last_state_id = 0;
    s_status.degraded_mic = false;
    s_status.overflow = false;
    s_note_set_stream_logged = false;
    taskEXIT_CRITICAL(&s_status_lock);
    return send_command_line(command, (size_t)length);
}

esp_err_t s3_bus_stop_stream(void)
{
    static const char command[] = "{\"cmd\":\"stop\"}\n";
    if (!bus_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&s_status_lock);
    const uint32_t sid = s_status.last_sid;
    const uint32_t sender_ts_ms = s_status.last_sender_ts_ms;
    const uint32_t seq = s_status.last_seq;
    taskEXIT_CRITICAL(&s_status_lock);
    if (!release_active_notes(sid, sender_ts_ms, seq)) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_note_lock, portMAX_DELAY);
    s_note_state_sid = 0;
    s_last_note_state_id = 0;
    xSemaphoreGive(s_note_lock);
    taskENTER_CRITICAL(&s_status_lock);
    s_status.stream_requested = false;
    s_requested_profile = S3_MUSIC_STREAM_PROFILE_PERFORMANCE;
    s_ack_state_id = 0;
    s_status.ack_state_id = 0;
    s_status.last_state_id = 0;
    s_status.degraded_mic = false;
    s_status.overflow = false;
    s_note_set_stream_logged = false;
    taskEXIT_CRITICAL(&s_status_lock);
    return send_command_line(command, sizeof(command) - 1U);
}

static esp_err_t sync_requested_stream(void)
{
    taskENTER_CRITICAL(&s_status_lock);
    const bool requested = s_status.stream_requested;
    const uint32_t sid = s_requested_sid;
    const s3_music_stream_profile_t profile = s_requested_profile;
    taskEXIT_CRITICAL(&s_status_lock);
    return requested ? s3_bus_start_stream_with_profile(sid, profile)
                     : s3_bus_stop_stream();
}

static bool accept_and_record_message(const s3_music_message_t *message)
{
    const uint32_t receive_ms = local_now_ms();
    bool accepted = true;

    taskENTER_CRITICAL(&s_status_lock);
    const bool first_valid_frame = !s_received_any;
    s_received_any = true;
    s_status.last_rx_ms = receive_ms;
    s_status.last_sender_ts_ms = message->ts_ms;

    const bool new_session =
        message->type == S3_MUSIC_MESSAGE_HELLO ||
        !s_sequence_valid ||
        message->sid != s_status.last_sid;
    if (!new_session &&
        (int32_t)(message->seq - s_status.last_seq) <= 0) {
        ++s_status.duplicate_frames;
        accepted = false;
    } else {
        s_sequence_valid = true;
        s_status.last_sid = message->sid;
        s_status.last_seq = message->seq;
        ++s_status.valid_frames;

        if (message->type == S3_MUSIC_MESSAGE_STATUS) {
            s_status.ready = message->ready;
            s_status.stream_enabled = message->stream_enabled;
        } else if (message->type == S3_MUSIC_MESSAGE_HEARTBEAT) {
            s_status.ready = message->ready;
        } else if (message->type == S3_MUSIC_MESSAGE_PONG) {
            s_status.command_link_confirmed = true;
            s_status.last_pong_ms = receive_ms;
        }
    }
    taskEXIT_CRITICAL(&s_status_lock);
    if (first_valid_frame) {
        ESP_LOGI(TAG, "RX path confirmed: first valid S3 frame received on GPIO%d",
                 BOARD_WT99_MUSIC_UART_RX_GPIO);
    }
    return accepted;
}

static void queue_music_event(const s3_music_message_t *message)
{
    s3_music_event_type_t event_type = S3_MUSIC_EVENT_NOTE_OFF;
    if (message->type == S3_MUSIC_MESSAGE_NOTE_ON) {
        event_type = S3_MUSIC_EVENT_NOTE_ON;
    } else if (message->type == S3_MUSIC_MESSAGE_POLY) {
        event_type = S3_MUSIC_EVENT_POLY;
    } else if (message->type == S3_MUSIC_MESSAGE_DIAGNOSTIC) {
        event_type = S3_MUSIC_EVENT_DIAGNOSTIC;
    }
    s3_music_event_t event = {
        .type = event_type,
        .seq = message->seq,
        .sid = message->sid,
        .sender_ts_ms = message->ts_ms,
        .duration_ms = message->duration_ms,
        .midi = message->midi,
        .velocity = message->velocity,
        .frequency_hz = message->frequency_hz,
        .confidence = message->confidence,
        .has_duration = message->has_duration,
        .has_frequency = message->has_frequency,
        .has_confidence = message->has_confidence,
        .poly_kind = message->poly_kind == S3_PROTOCOL_POLY_INTERVAL
                         ? S3_MUSIC_POLY_INTERVAL
                         : message->poly_kind == S3_PROTOCOL_POLY_CHORD
                               ? S3_MUSIC_POLY_CHORD
                               : S3_MUSIC_POLY_NONE,
        .note_count = message->note_count,
        .diagnostic_raw_count = message->diagnostic_raw_count,
        .diagnostic_candidate_kind =
            (s3_music_result_kind_t)message->diagnostic_candidate_kind,
        .diagnostic_candidate_count =
            message->diagnostic_candidate_count,
        .diagnostic_final_kind =
            (s3_music_result_kind_t)message->diagnostic_final_kind,
        .diagnostic_final_count = message->diagnostic_final_count,
        .diagnostic_octave_shift = message->diagnostic_octave_shift,
    };
    memcpy(event.notes, message->notes, sizeof(event.notes));
    memcpy(event.poly_name, message->poly_name, sizeof(event.poly_name));
    for (int index = 0; index < message->diagnostic_raw_count; ++index) {
        event.diagnostic_raw[index].source =
            message->diagnostic_raw[index].source;
        event.diagnostic_raw[index].midi =
            message->diagnostic_raw[index].midi;
        event.diagnostic_raw[index].frequency_hz =
            message->diagnostic_raw[index].frequency_hz;
        event.diagnostic_raw[index].confidence =
            message->diagnostic_raw[index].confidence;
    }
    memcpy(event.diagnostic_candidate_notes,
           message->diagnostic_candidate_notes,
           sizeof(event.diagnostic_candidate_notes));
    memcpy(event.diagnostic_final_notes,
           message->diagnostic_final_notes,
           sizeof(event.diagnostic_final_notes));
    memcpy(event.diagnostic_snr_db, message->diagnostic_snr_db,
           sizeof(event.diagnostic_snr_db));
    memcpy(event.diagnostic_reject, message->diagnostic_reject,
           sizeof(event.diagnostic_reject));
    xSemaphoreTake(s_note_lock, portMAX_DELAY);
    (void)queue_event_batch(&event, 1);
    xSemaphoreGive(s_note_lock);
}

static void reset_note_session(uint32_t sender_ts_ms, uint32_t seq)
{
    xSemaphoreTake(s_note_lock, portMAX_DELAY);
    const uint32_t active_sid = s_note_state_sid;
    xSemaphoreGive(s_note_lock);
    if (!release_active_notes(active_sid, sender_ts_ms, seq)) return;

    xSemaphoreTake(s_note_lock, portMAX_DELAY);
    s_note_state_sid = 0;
    s_last_note_state_id = 0;
    xSemaphoreGive(s_note_lock);
    taskENTER_CRITICAL(&s_status_lock);
    s_ack_state_id = 0;
    s_status.ack_state_id = 0;
    s_status.last_state_id = 0;
    s_status.degraded_mic = false;
    s_status.overflow = false;
    taskEXIT_CRITICAL(&s_status_lock);
}

static void process_note_snapshot(const s3_music_message_t *message)
{
    xSemaphoreTake(s_note_lock, portMAX_DELAY);
    const uint32_t active_sid = s_note_state_sid;
    xSemaphoreGive(s_note_lock);
    if (message->sid != active_sid) {
        if (!release_active_notes(active_sid, message->ts_ms,
                                  message->seq)) {
            return;
        }
        xSemaphoreTake(s_note_lock, portMAX_DELAY);
        s_note_state_sid = message->sid;
        s_last_note_state_id = 0;
        xSemaphoreGive(s_note_lock);
        taskENTER_CRITICAL(&s_status_lock);
        s_ack_state_id = 0;
        s_status.ack_state_id = 0;
        s_status.last_state_id = 0;
        taskEXIT_CRITICAL(&s_status_lock);
    }

    xSemaphoreTake(s_note_lock, portMAX_DELAY);
    if (message->state_id <= s_last_note_state_id) {
        const uint32_t acknowledged = s_last_note_state_id;
        xSemaphoreGive(s_note_lock);
        taskENTER_CRITICAL(&s_status_lock);
        s_ack_state_id = acknowledged;
        s_status.ack_state_id = acknowledged;
        taskEXIT_CRITICAL(&s_status_lock);
        return;
    }

    s3_music_event_t events[S3_MUSIC_NOTE_SET_MAX_NOTES * 2];
    size_t event_count = 0;
    for (uint8_t active = 0; active < s_active_note_count; ++active) {
        const uint8_t midi = s_active_midis[active];
        bool retained = false;
        for (uint8_t next = 0; next < message->note_set_count; ++next) {
            retained = retained || message->midis[next] == midi;
        }
        if (!retained) {
            events[event_count++] = (s3_music_event_t) {
                .type = S3_MUSIC_EVENT_NOTE_OFF,
                .seq = message->seq,
                .sid = message->sid,
                .sender_ts_ms = message->ts_ms,
                .duration_ms = sender_elapsed_ms(message->ts_ms,
                                                 s_note_on_ts[midi]),
                .midi = midi,
                .velocity = 0,
                .has_duration = true,
            };
        }
    }
    for (uint8_t next = 0; next < message->note_set_count; ++next) {
        const uint8_t midi = message->midis[next];
        if (active_index(midi) >= 0) continue;
        events[event_count++] = (s3_music_event_t) {
            .type = S3_MUSIC_EVENT_NOTE_ON,
            .seq = message->seq,
            .sid = message->sid,
            .sender_ts_ms = message->ts_ms,
            .midi = midi,
            .velocity = message->velocities[next],
            .frequency_hz = 440.0f *
                powf(2.0f, ((float)midi - 69.0f) / 12.0f),
            .confidence = message->confidences[next],
            .has_frequency = true,
            .has_confidence = true,
        };
    }
    if (!queue_event_batch(events, event_count)) {
        xSemaphoreGive(s_note_lock);
        return;
    }

    uint32_t retained_on_ts[S3_MUSIC_NOTE_SET_MAX_NOTES] = {0};
    for (uint8_t next = 0; next < message->note_set_count; ++next) {
        const uint8_t midi = message->midis[next];
        retained_on_ts[next] = active_index(midi) >= 0
                                   ? s_note_on_ts[midi] : message->ts_ms;
    }
    memset(s_active_midis, 0, sizeof(s_active_midis));
    memset(s_note_on_ts, 0, sizeof(s_note_on_ts));
    s_active_note_count = message->note_set_count;
    for (uint8_t next = 0; next < message->note_set_count; ++next) {
        const uint8_t midi = message->midis[next];
        s_active_midis[next] = midi;
        s_note_on_ts[midi] = retained_on_ts[next];
    }
    s_last_note_state_id = message->state_id;
    xSemaphoreGive(s_note_lock);

    taskENTER_CRITICAL(&s_status_lock);
    s_ack_state_id = message->state_id;
    s_status.ack_state_id = message->state_id;
    s_status.last_state_id = message->state_id;
    s_status.active_note_count = message->note_set_count;
    s_status.degraded_mic = message->degraded_mic;
    s_status.overflow = message->overflow;
    taskEXIT_CRITICAL(&s_status_lock);

    taskENTER_CRITICAL(&s_status_lock);
    const bool first_note_set = !s_note_set_stream_logged;
    s_note_set_stream_logged = true;
    taskEXIT_CRITICAL(&s_status_lock);
    if (first_note_set) {
        ESP_LOGI(TAG,
                 "MusicLink v2 note-set confirmed: sid=%" PRIu32
                 " state=%" PRIu32 " notes=%u degraded=%d overflow=%d",
                 message->sid, message->state_id,
                 (unsigned)message->note_set_count,
                 message->degraded_mic, message->overflow);
    }
}

static void process_line(char *line, size_t length)
{
    line[length] = '\0';
    s3_music_message_t message;
    const s3_protocol_result_t result =
        s3_protocol_parse_music_line(line, length, &message);
    if (result != S3_PROTOCOL_OK) {
        increment_invalid();
        return;
    }
    if (message.type == S3_MUSIC_MESSAGE_NOTES ||
        message.type == S3_MUSIC_MESSAGE_NOTE_ON ||
        message.type == S3_MUSIC_MESSAGE_NOTE_OFF) {
        taskENTER_CRITICAL(&s_status_lock);
        const bool current_note_session = s_status.stream_requested &&
                                          message.sid == s_requested_sid;
        if (!current_note_session) ++s_status.duplicate_frames;
        taskEXIT_CRITICAL(&s_status_lock);
        if (!current_note_session) return;
    }
    if (!accept_and_record_message(&message)) {
        return;
    }
    taskENTER_CRITICAL(&s_status_lock);
    const bool recovered_after_timeout = s_timeout_released;
    if (recovered_after_timeout) s_timeout_released = false;
    taskEXIT_CRITICAL(&s_status_lock);
    if (recovered_after_timeout &&
        message.type != S3_MUSIC_MESSAGE_HELLO) {
        const esp_err_t recover_err = sync_requested_stream();
        if (recover_err != ESP_OK) {
            ESP_LOGW(TAG, "failed to restore stream after timeout: %s",
                     esp_err_to_name(recover_err));
        }
        if (message.type == S3_MUSIC_MESSAGE_NOTES) return;
    }
    if (message.type == S3_MUSIC_MESSAGE_HELLO) {
        reset_note_session(message.ts_ms, message.seq);
        esp_err_t err = sync_requested_stream();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to synchronize S3 stream state: %s",
                     esp_err_to_name(err));
        }
        err = s3_bus_ping();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to ping S3: %s", esp_err_to_name(err));
        }
    } else if (message.type == S3_MUSIC_MESSAGE_PONG) {
        ESP_LOGI(TAG, "bidirectional UART confirmed by S3 pong (sid=%" PRIu32
                      ", seq=%" PRIu32 ")",
                 message.sid, message.seq);
    }
    if (message.type == S3_MUSIC_MESSAGE_NOTES) {
        process_note_snapshot(&message);
        return;
    }
    if (message.type == S3_MUSIC_MESSAGE_NOTE_ON ||
        message.type == S3_MUSIC_MESSAGE_NOTE_OFF ||
        message.type == S3_MUSIC_MESSAGE_POLY ||
        message.type == S3_MUSIC_MESSAGE_DIAGNOSTIC) {
        if (message.type == S3_MUSIC_MESSAGE_DIAGNOSTIC) {
            queue_music_event(&message);
            return;
        }
        if (!s_note_stream_logged) {
            const char *type =
                message.type == S3_MUSIC_MESSAGE_NOTE_ON ? "note_on" :
                message.type == S3_MUSIC_MESSAGE_NOTE_OFF ? "note_off" :
                                                            "poly";
            ESP_LOGI(TAG, "S3 music stream confirmed: type=%s", type);
            s_note_stream_logged = true;
        }
        if (message.type == S3_MUSIC_MESSAGE_POLY &&
            !s_poly_stream_logged) {
            ESP_LOGI(TAG,
                     "native poly stream confirmed: kind=%s notes=%u name=%s",
                     message.poly_kind == S3_PROTOCOL_POLY_CHORD
                         ? "chord" : "interval",
                     (unsigned)message.note_count,
                     message.poly_name[0] ? message.poly_name : "-");
            s_poly_stream_logged = true;
        }
        queue_music_event(&message);
    }
}

static void music_rx_task(void *argument)
{
    (void)argument;
    uint8_t input[64];
    char line[S3_MUSIC_LINE_MAX_BYTES + 1];
    size_t line_length = 0;
    bool overflow = false;

    while (true) {
        const int received = uart_read_bytes(
            (uart_port_t)BOARD_WT99_MUSIC_UART_PORT,
            input, sizeof(input), pdMS_TO_TICKS(50));
        if (received <= 0) {
            continue;
        }
        taskENTER_CRITICAL(&s_status_lock);
        const bool first_uart_bytes = s_status.rx_bytes == 0;
        s_status.rx_bytes += (uint32_t)received;
        taskEXIT_CRITICAL(&s_status_lock);
        if (first_uart_bytes) {
            ESP_LOGI(TAG, "electrical RX activity detected on GPIO%d (%d bytes)",
                     BOARD_WT99_MUSIC_UART_RX_GPIO, received);
        }
        for (int i = 0; i < received; ++i) {
            const char character = (char)input[i];
            if (character == '\n') {
                if (overflow) {
                    increment_oversized();
                } else if (line_length > 0) {
                    process_line(line, line_length);
                }
                line_length = 0;
                overflow = false;
            } else if (character != '\r' && !overflow) {
                if (line_length < S3_MUSIC_LINE_MAX_BYTES) {
                    line[line_length++] = character;
                } else {
                    overflow = true;
                }
            }
        }
    }
}

static void music_dispatch_task(void *argument)
{
    (void)argument;
    s3_music_event_t event;
    while (true) {
        if (xQueueReceive(s_event_queue, &event, portMAX_DELAY) == pdTRUE &&
            s_handler != NULL) {
            s_handler(&event, s_handler_context);
        }
    }
}

static void cleanup_failed_init(void)
{
    if (s_poll_task != NULL) {
        vTaskDelete(s_poll_task);
        s_poll_task = NULL;
    }
    if (s_rx_task != NULL) {
        vTaskDelete(s_rx_task);
        s_rx_task = NULL;
    }
    if (s_dispatch_task != NULL) {
        vTaskDelete(s_dispatch_task);
        s_dispatch_task = NULL;
    }
    if (s_event_queue != NULL) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
    }
    if (s_tx_lock != NULL) {
        vSemaphoreDelete(s_tx_lock);
        s_tx_lock = NULL;
    }
    if (s_note_lock != NULL) {
        vSemaphoreDelete(s_note_lock);
        s_note_lock = NULL;
    }
    uart_driver_delete((uart_port_t)BOARD_WT99_MUSIC_UART_PORT);
    s_handler = NULL;
    s_handler_context = NULL;
}

esp_err_t s3_bus_init(s3_music_event_handler_t handler, void *context)
{
    taskENTER_CRITICAL(&s_status_lock);
    const bool initialized = s_status.initialized;
    taskEXIT_CRITICAL(&s_status_lock);
    if (initialized) {
        return ESP_OK;
    }
    if (handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_handler = handler;
    s_handler_context = context;
    s_tx_lock = xSemaphoreCreateMutex();
    s_note_lock = xSemaphoreCreateMutex();
    s_event_queue = xQueueCreate(S3_MUSIC_EVENT_QUEUE_LENGTH,
                                 sizeof(s3_music_event_t));
    if (s_tx_lock == NULL || s_note_lock == NULL || s_event_queue == NULL) {
        cleanup_failed_init();
        return ESP_ERR_NO_MEM;
    }

    const uart_config_t uart_config = {
        .baud_rate = BOARD_WT99_MUSIC_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {0},
    };
    esp_err_t err = uart_driver_install(
        (uart_port_t)BOARD_WT99_MUSIC_UART_PORT,
        S3_MUSIC_UART_RX_BUFFER_BYTES,
        S3_MUSIC_UART_TX_BUFFER_BYTES, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(
            (uart_port_t)BOARD_WT99_MUSIC_UART_PORT, &uart_config);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(
            (uart_port_t)BOARD_WT99_MUSIC_UART_PORT,
            BOARD_WT99_MUSIC_UART_TX_GPIO,
            BOARD_WT99_MUSIC_UART_RX_GPIO,
            UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        cleanup_failed_init();
        return err;
    }
    uart_flush_input((uart_port_t)BOARD_WT99_MUSIC_UART_PORT);

    if (xTaskCreate(music_dispatch_task, "s3_music_dispatch",
                    S3_MUSIC_DISPATCH_STACK_BYTES, NULL,
                    S3_MUSIC_DISPATCH_PRIORITY,
                    &s_dispatch_task) != pdPASS ||
        xTaskCreate(music_rx_task, "s3_music_rx",
                    S3_MUSIC_RX_TASK_STACK_BYTES, NULL,
                    S3_MUSIC_RX_TASK_PRIORITY,
                    &s_rx_task) != pdPASS ||
        xTaskCreate(music_poll_task, "s3_music_poll",
                    S3_MUSIC_POLL_STACK_BYTES, NULL,
                    S3_MUSIC_POLL_PRIORITY,
                    &s_poll_task) != pdPASS) {
        cleanup_failed_init();
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_status_lock);
    s_status.initialized = true;
    taskEXIT_CRITICAL(&s_status_lock);
    ESP_LOGI(TAG, "master-poll UART%d TX=GPIO%d RX=GPIO%d at %d baud (%d ms)",
             BOARD_WT99_MUSIC_UART_PORT,
             BOARD_WT99_MUSIC_UART_TX_GPIO,
             BOARD_WT99_MUSIC_UART_RX_GPIO,
             BOARD_WT99_MUSIC_UART_BAUD_RATE,
             S3_MUSIC_POLL_INTERVAL_MS);

    /* The S3 buffers all uplink frames until this P4 polls it. Keep recognition
     * quiet until audio_s3 is selected, then verify the command path by pong. */
    (void)sync_requested_stream();
    (void)s3_bus_ping();
    return ESP_OK;
}

void s3_bus_get_status(s3_bus_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_status_lock);
    *out_status = s_status;
    const bool received_any = s_received_any;
    taskEXIT_CRITICAL(&s_status_lock);

    out_status->online =
        out_status->initialized && received_any &&
        (uint32_t)(local_now_ms() - out_status->last_rx_ms) <=
            S3_MUSIC_ONLINE_TIMEOUT_MS;
}
