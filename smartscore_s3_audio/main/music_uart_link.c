#include "music_uart_link.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board_pins.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "music_detector_config.h"

#define MUSIC_LINK_JSON_BUFFER_SIZE 384
#define MUSIC_LINK_RX_LINE_SIZE     160
#define MUSIC_LINK_OUTPUT_QUEUE_LENGTH 16
#define MUSIC_LINK_MAX_FRAMES_PER_POLL 4
#define MUSIC_LINK_TX_DONE_TIMEOUT_MS  100
#define MUSIC_LINK_DIAG_INTERVAL_MS     5000
#define MUSIC_LINK_RX_SAMPLE_SIZE         32
#define MUSIC_LINK_MELODY_MIN_MIDI         36
#define MUSIC_LINK_MELODY_MAX_MIDI         96
#define MUSIC_LINK_MELODY_CENTER_MIDI      69
#define MUSIC_LINK_MELODY_YIN_CONFIDENCE 0.45f

typedef struct {
    uint16_t length;
    char data[MUSIC_LINK_JSON_BUFFER_SIZE];
} link_output_frame_t;

typedef enum {
    LINK_STATE_RESULT = 0,
    LINK_STATE_READY,
} link_state_event_type_t;

typedef struct {
    link_state_event_type_t type;
    union {
        music_result_t result;
        bool ready;
    } data;
} link_state_event_t;

typedef struct {
    uint32_t timestamp_ms;
    int midi;
    float frequency_hz;
    float cents;
    float confidence;
    float rms;
} link_pitch_event_t;

typedef struct {
    uint32_t seq;
    uint32_t sid;
    bool ready;
    bool stream_enabled;
    bool active_note;
    int active_midi;
    uint32_t note_on_ms;
    uint32_t last_note_seen_ms;
    uint32_t unknown_since_ms;
    bool poly_active;
    bool poly_melody_logged;
    music_result_type_t poly_type;
    int poly_note_count;
    int poly_notes[4];
    char poly_name[16];
    uint32_t last_pitch_ms;
    uint32_t last_heartbeat_ms;
    uint32_t last_link_diag_ms;
    uint32_t rx_bytes;
    uint8_t rx_sample[MUSIC_LINK_RX_SAMPLE_SIZE];
    size_t rx_sample_length;
    bool rx_sample_logged;
    uint32_t poll_count;
    uint32_t response_frames;
    uint32_t invalid_commands;
    char rx_line[MUSIC_LINK_RX_LINE_SIZE];
    size_t rx_length;
    bool rx_overflow;
} music_link_state_t;

static const char *const TAG = "MUSIC_LINK";
static QueueHandle_t s_state_queue;
static QueueHandle_t s_pitch_queue;
static QueueHandle_t s_output_queue;
static portMUX_TYPE s_drop_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_tx_drop;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void increment_drop(void)
{
    portENTER_CRITICAL(&s_drop_lock);
    ++s_tx_drop;
    portEXIT_CRITICAL(&s_drop_lock);
}

static uint32_t read_drop(void)
{
    uint32_t value;
    portENTER_CRITICAL(&s_drop_lock);
    value = s_tx_drop;
    portEXIT_CRITICAL(&s_drop_lock);
    return value;
}

static float finite_or_zero(float value)
{
    return isfinite(value) ? value : 0.0f;
}

static float confidence_value(float value)
{
    value = finite_or_zero(value);
    return fminf(fmaxf(value, 0.0f), 1.0f);
}

static int velocity_from_rms(float rms)
{
    const float normalized = fminf(fmaxf(finite_or_zero(rms), 0.0f) /
                                   MUSIC_UART_VELOCITY_FULL_SCALE_RMS, 1.0f);
    const int velocity = (int)lrintf(127.0f * sqrtf(normalized));
    return velocity < 1 ? 1 : (velocity > 127 ? 127 : velocity);
}

static uint32_t next_seq(music_link_state_t *state)
{
    ++state->seq;
    if (state->seq == 0) ++state->seq;
    return state->seq;
}

static void send_checked_json(char *json, size_t capacity, int length)
{
    if (length < 0 || (size_t)length >= capacity - 1U) {
        increment_drop();
        return;
    }
    json[length++] = '\n';
    json[length] = '\0';

    link_output_frame_t frame = {
        .length = (uint16_t)length,
    };
    memcpy(frame.data, json, (size_t)length);
    if (s_output_queue == NULL ||
        xQueueSend(s_output_queue, &frame, 0) != pdTRUE) {
        increment_drop();
    }
}

static void send_hello(music_link_state_t *state, uint32_t timestamp_ms)
{
    char json[MUSIC_LINK_JSON_BUFFER_SIZE];
    const int length = snprintf(json, sizeof(json) - 1U,
        "{\"v\":1,\"type\":\"hello\",\"seq\":%" PRIu32 ",\"sid\":%" PRIu32
        ",\"ts_ms\":%" PRIu32 ",\"source\":\"s3_audio\",\"sample_rate\":%d,"
        "\"a4\":%.1f,\"mic\":\"ch1\"}",
        next_seq(state), state->sid, timestamp_ms, MUSIC_SAMPLE_RATE_HZ,
        (double)MUSIC_REFERENCE_A4_HZ);
    send_checked_json(json, sizeof(json), length);
}

static void send_status(music_link_state_t *state, uint32_t timestamp_ms, const char *status)
{
    char json[MUSIC_LINK_JSON_BUFFER_SIZE];
    const int length = snprintf(json, sizeof(json) - 1U,
        "{\"v\":1,\"type\":\"status\",\"seq\":%" PRIu32 ",\"sid\":%" PRIu32
        ",\"ts_ms\":%" PRIu32 ",\"state\":\"%s\",\"ready\":%s,\"stream_enabled\":%s}",
        next_seq(state), state->sid, timestamp_ms, status,
        state->ready ? "true" : "false", state->stream_enabled ? "true" : "false");
    send_checked_json(json, sizeof(json), length);
}

static void send_pong(music_link_state_t *state, uint32_t timestamp_ms)
{
    char json[MUSIC_LINK_JSON_BUFFER_SIZE];
    const int length = snprintf(json, sizeof(json) - 1U,
        "{\"v\":1,\"type\":\"pong\",\"seq\":%" PRIu32 ",\"sid\":%" PRIu32
        ",\"ts_ms\":%" PRIu32 "}",
        next_seq(state), state->sid, timestamp_ms);
    send_checked_json(json, sizeof(json), length);
}

static void send_heartbeat(music_link_state_t *state, uint32_t timestamp_ms)
{
    char json[MUSIC_LINK_JSON_BUFFER_SIZE];
    const int length = snprintf(json, sizeof(json) - 1U,
        "{\"v\":1,\"type\":\"heartbeat\",\"seq\":%" PRIu32 ",\"sid\":%" PRIu32
        ",\"ts_ms\":%" PRIu32 ",\"ready\":%s,\"tx_drop\":%" PRIu32 "}",
        next_seq(state), state->sid, timestamp_ms, state->ready ? "true" : "false", read_drop());
    send_checked_json(json, sizeof(json), length);
}

static void send_pitch(music_link_state_t *state, const link_pitch_event_t *pitch)
{
    if (!state->stream_enabled || pitch->midi < 0 || pitch->midi > 127 ||
        pitch->frequency_hz <= 0.0f || !isfinite(pitch->frequency_hz)) {
        return;
    }
    char json[MUSIC_LINK_JSON_BUFFER_SIZE];
    const int length = snprintf(json, sizeof(json) - 1U,
        "{\"v\":1,\"type\":\"pitch\",\"seq\":%" PRIu32 ",\"sid\":%" PRIu32
        ",\"ts_ms\":%" PRIu32 ",\"midi\":%d,\"freq_hz\":%.2f,\"cents\":%.2f,"
        "\"confidence\":%.3f,\"rms\":%.5f}",
        next_seq(state), state->sid, pitch->timestamp_ms, pitch->midi,
        (double)finite_or_zero(pitch->frequency_hz), (double)finite_or_zero(pitch->cents),
        (double)confidence_value(pitch->confidence), (double)fmaxf(finite_or_zero(pitch->rms), 0.0f));
    send_checked_json(json, sizeof(json), length);
}

static void send_note_on(music_link_state_t *state, const music_result_t *result)
{
    char json[MUSIC_LINK_JSON_BUFFER_SIZE];
    const int length = snprintf(json, sizeof(json) - 1U,
        "{\"v\":1,\"type\":\"note_on\",\"seq\":%" PRIu32 ",\"sid\":%" PRIu32
        ",\"ts_ms\":%" PRIu32 ",\"midi\":%d,\"velocity\":%d,\"freq_hz\":%.2f,"
        "\"confidence\":%.3f}",
        next_seq(state), state->sid, result->timestamp_ms, result->midi,
        velocity_from_rms(result->rms), (double)fmaxf(finite_or_zero(result->frequency_hz), 0.0f),
        (double)confidence_value(result->confidence));
    send_checked_json(json, sizeof(json), length);
}

static void close_active_note(music_link_state_t *state, uint32_t timestamp_ms, const char *reason)
{
    if (!state->active_note) return;
    const uint32_t duration_ms = timestamp_ms - state->note_on_ms;
    char json[MUSIC_LINK_JSON_BUFFER_SIZE];
    const int length = snprintf(json, sizeof(json) - 1U,
        "{\"v\":1,\"type\":\"note_off\",\"seq\":%" PRIu32 ",\"sid\":%" PRIu32
        ",\"ts_ms\":%" PRIu32 ",\"midi\":%d,\"duration_ms\":%" PRIu32
        ",\"reason\":\"%s\"}",
        next_seq(state), state->sid, timestamp_ms, state->active_midi, duration_ms, reason);
    send_checked_json(json, sizeof(json), length);
    state->active_note = false;
    state->active_midi = -1;
    state->unknown_since_ms = 0;
}

static bool append_note(char *output, size_t capacity, size_t *used, int midi, bool prepend_comma)
{
    if (midi < 0 || midi > 127 || *used >= capacity) return false;
    const int length = snprintf(output + *used, capacity - *used, "%s%d",
                                prepend_comma ? "," : "", midi);
    if (length < 0 || (size_t)length >= capacity - *used) return false;
    *used += (size_t)length;
    return true;
}

static bool build_notes_array(const music_result_t *result, char *output, size_t capacity)
{
    if (capacity < 3 || result->pitch_class_count < 2 || result->pitch_class_count > 4) return false;
    size_t used = 0;
    output[used++] = '[';
    for (int i = 0; i < result->pitch_class_count; ++i) {
        if (!append_note(output, capacity, &used, result->midi_notes[i], i != 0)) return false;
    }
    if (used + 2 > capacity) return false;
    output[used++] = ']';
    output[used] = '\0';
    return true;
}

static void send_poly(music_link_state_t *state, const music_result_t *result)
{
    char notes[40];
    if (!build_notes_array(result, notes, sizeof(notes))) {
        increment_drop();
        return;
    }
    char json[MUSIC_LINK_JSON_BUFFER_SIZE];
    int length;
    if (result->type == MUSIC_RESULT_INTERVAL) {
        length = snprintf(json, sizeof(json) - 1U,
            "{\"v\":1,\"type\":\"poly\",\"kind\":\"interval\",\"seq\":%" PRIu32
            ",\"sid\":%" PRIu32 ",\"ts_ms\":%" PRIu32 ",\"notes\":%s,\"confidence\":%.3f}",
            next_seq(state), state->sid, result->timestamp_ms, notes,
            (double)confidence_value(result->confidence));
    } else {
        length = snprintf(json, sizeof(json) - 1U,
            "{\"v\":1,\"type\":\"poly\",\"kind\":\"chord\",\"seq\":%" PRIu32
            ",\"sid\":%" PRIu32 ",\"ts_ms\":%" PRIu32 ",\"name\":\"%s\","
            "\"notes\":%s,\"confidence\":%.3f}",
            next_seq(state), state->sid, result->timestamp_ms, result->chord_name, notes,
            (double)confidence_value(result->confidence));
    }
    send_checked_json(json, sizeof(json), length);
}

static bool same_poly(const music_link_state_t *state, const music_result_t *result)
{
    if (!state->poly_active || state->poly_type != result->type ||
        state->poly_note_count != result->pitch_class_count) return false;
    if (result->type == MUSIC_RESULT_CHORD &&
        strncmp(state->poly_name, result->chord_name, sizeof(state->poly_name)) != 0) return false;
    for (int i = 0; i < result->pitch_class_count; ++i) {
        if (state->poly_notes[i] != result->midi_notes[i]) return false;
    }
    return true;
}

static void remember_poly(music_link_state_t *state, const music_result_t *result)
{
    state->poly_active = true;
    state->poly_type = result->type;
    state->poly_note_count = result->pitch_class_count;
    memcpy(state->poly_notes, result->midi_notes, sizeof(state->poly_notes));
    snprintf(state->poly_name, sizeof(state->poly_name), "%s", result->chord_name);
}

static bool poly_contains_pitch_class(const music_result_t *result, int midi)
{
    if (midi < 0) return false;
    const int pitch_class = midi % 12;
    for (int i = 0; i < result->pitch_class_count; ++i) {
        if (result->pitch_classes[i] == pitch_class) return true;
    }
    return false;
}

static int melody_midi_near_reference(int midi, int reference)
{
    if (midi < 0 || midi > 127) return -1;
    while (midi < MUSIC_LINK_MELODY_MIN_MIDI) midi += 12;
    while (midi > MUSIC_LINK_MELODY_MAX_MIDI) midi -= 12;
    while (midi + 12 <= MUSIC_LINK_MELODY_MAX_MIDI &&
           abs((midi + 12) - reference) < abs(midi - reference)) {
        midi += 12;
    }
    while (midi - 12 >= MUSIC_LINK_MELODY_MIN_MIDI &&
           abs((midi - 12) - reference) < abs(midi - reference)) {
        midi -= 12;
    }
    return midi;
}

static int select_poly_melody_midi(const music_link_state_t *state,
                                   const music_result_t *result)
{
    const int reference = state->active_note
                              ? state->active_midi
                              : MUSIC_LINK_MELODY_CENTER_MIDI;
    if (result->midi >= 0 && result->midi <= 127 &&
        result->yin_confidence >= MUSIC_LINK_MELODY_YIN_CONFIDENCE &&
        poly_contains_pitch_class(result, result->midi)) {
        return melody_midi_near_reference(result->midi, reference);
    }

    int selected = -1;
    int selected_distance = INT_MAX;
    for (int i = 0; i < result->pitch_class_count; ++i) {
        const int candidate = melody_midi_near_reference(result->midi_notes[i],
                                                         reference);
        if (candidate < 0) continue;
        const int distance = abs(candidate - reference);
        if (distance < selected_distance ||
            (distance == selected_distance && candidate > selected)) {
            selected = candidate;
            selected_distance = distance;
        }
    }
    return selected;
}

static void update_active_melody_note(music_link_state_t *state,
                                      const music_result_t *result,
                                      int midi)
{
    if (midi < 0 || midi > 127) return;
    if (state->active_note) {
        if (state->active_midi != midi) {
            close_active_note(state, result->timestamp_ms, "changed");
        } else if (result->onset &&
                   result->timestamp_ms - state->note_on_ms >=
                       MUSIC_UART_SAME_NOTE_RETRIGGER_MS) {
            /* A new amplitude attack at the same pitch is a repeated note,
             * not one long held note. Keep the existing NDJSON protocol and
             * express it as note_off followed by note_on. */
            close_active_note(state, result->timestamp_ms, "retrigger");
        }
    }
    if (!state->active_note) {
        music_result_t melody = *result;
        melody.type = MUSIC_RESULT_SINGLE;
        melody.midi = midi;
        melody.frequency_hz = 440.0f * powf(2.0f, (float)(midi - 69) / 12.0f);
        melody.confidence = fmaxf(confidence_value(result->confidence),
                                  confidence_value(result->yin_confidence));
        send_note_on(state, &melody);
        state->active_note = true;
        state->active_midi = midi;
        state->note_on_ms = result->timestamp_ms;
    }
    state->last_note_seen_ms = result->timestamp_ms;
}

static void process_result(music_link_state_t *state, const music_result_t *result)
{
    if (!state->stream_enabled) return;
    switch (result->type) {
        case MUSIC_RESULT_SINGLE:
            state->unknown_since_ms = 0;
            state->poly_active = false;
            update_active_melody_note(state, result, result->midi);
            break;
        case MUSIC_RESULT_SILENCE:
            state->poly_active = false;
            state->unknown_since_ms = 0;
            close_active_note(state, result->timestamp_ms, "silence");
            break;
        case MUSIC_RESULT_INTERVAL:
        case MUSIC_RESULT_CHORD: {
            state->unknown_since_ms = 0;
            if (!same_poly(state, result)) {
                send_poly(state, result);
                remember_poly(state, result);
            }
            const int melody_midi = select_poly_melody_midi(state, result);
            if (melody_midi >= 0) {
                if (!state->poly_melody_logged) {
                    ESP_LOGI(TAG, "poly-to-melody enabled: first MIDI=%d", melody_midi);
                    state->poly_melody_logged = true;
                }
                update_active_melody_note(state, result, melody_midi);
            }
            break;
        }
        case MUSIC_RESULT_UNKNOWN:
        default:
            if (!state->active_note) return;
            if (state->unknown_since_ms == 0) state->unknown_since_ms = result->timestamp_ms;
            if (result->timestamp_ms - state->unknown_since_ms >= MUSIC_UART_UNKNOWN_TIMEOUT_MS) {
                close_active_note(state, result->timestamp_ms, "timeout");
            }
            break;
    }
}

static bool extract_command(const char *line, char *command, size_t capacity)
{
    const char *cursor = strstr(line, "\"cmd\"");
    if (cursor == NULL) return false;
    cursor += strlen("\"cmd\"");
    while (isspace((unsigned char)*cursor)) ++cursor;
    if (*cursor++ != ':') return false;
    while (isspace((unsigned char)*cursor)) ++cursor;
    if (*cursor++ != '\"') return false;
    size_t used = 0;
    while (*cursor != '\0' && *cursor != '\"') {
        if (*cursor == '\\' || (unsigned char)*cursor < 0x20 || used + 1 >= capacity) return false;
        command[used++] = *cursor++;
    }
    if (*cursor != '\"') return false;
    command[used] = '\0';
    return used > 0;
}

static bool extract_sid(const char *line, uint32_t *sid)
{
    const char *cursor = strstr(line, "\"sid\"");
    if (cursor == NULL) return false;
    cursor += strlen("\"sid\"");
    while (isspace((unsigned char)*cursor)) ++cursor;
    if (*cursor++ != ':') return false;
    while (isspace((unsigned char)*cursor)) ++cursor;
    if (!isdigit((unsigned char)*cursor)) return false;
    errno = 0;
    char *end = NULL;
    const unsigned long value = strtoul(cursor, &end, 10);
    if (errno == ERANGE || end == cursor || value > UINT32_MAX) return false;
    while (isspace((unsigned char)*end)) ++end;
    if (*end != ',' && *end != '}') return false;
    *sid = (uint32_t)value;
    return true;
}

static void reset_poly_state(music_link_state_t *state)
{
    state->poly_active = false;
    state->poly_note_count = 0;
    state->poly_name[0] = '\0';
}

static void flush_poll_response(music_link_state_t *state)
{
    if (s_output_queue == NULL) return;

    ++state->poll_count;
    if (state->poll_count == 1) {
        ESP_LOGI(TAG, "RX path confirmed: first P4 poll received on GPIO%d (%" PRIu32
                      " UART bytes)",
                 (int)BOARD_MUSIC_LINK_RX_GPIO, state->rx_bytes);
    }

    const uint32_t timestamp_ms = now_ms();
    if (uxQueueMessagesWaiting(s_output_queue) == 0) {
        state->last_heartbeat_ms = timestamp_ms;
        send_heartbeat(state, timestamp_ms);
    }

    link_output_frame_t frame;
    unsigned sent = 0;
    unsigned written_frames = 0;
    while (sent < MUSIC_LINK_MAX_FRAMES_PER_POLL &&
           xQueueReceive(s_output_queue, &frame, 0) == pdTRUE) {
        const int written = uart_write_bytes(
            BOARD_MUSIC_LINK_UART_PORT, frame.data, frame.length);
        if (written != frame.length) {
            increment_drop();
        } else {
            ++written_frames;
        }
        ++sent;
    }
    if (sent > 0 &&
        uart_wait_tx_done(BOARD_MUSIC_LINK_UART_PORT,
                          pdMS_TO_TICKS(MUSIC_LINK_TX_DONE_TIMEOUT_MS)) != ESP_OK) {
        increment_drop();
    }
    const bool first_response = state->response_frames == 0 && written_frames > 0;
    state->response_frames += written_frames;
    if (first_response) {
        ESP_LOGI(TAG, "TX path active: first poll response transmitted on GPIO%d",
                 (int)BOARD_MUSIC_LINK_TX_GPIO);
    }
}

static void process_command(music_link_state_t *state, const char *line)
{
    const uint32_t timestamp_ms = now_ms();
    char command[20];
    if (!extract_command(line, command, sizeof(command))) {
        ++state->invalid_commands;
        send_status(state, timestamp_ms, "invalid_command");
        return;
    }
    if (strcmp(command, "poll") == 0) {
        flush_poll_response(state);
    } else if (strcmp(command, "ping") == 0) {
        send_pong(state, timestamp_ms);
    } else if (strcmp(command, "start") == 0) {
        uint32_t new_sid;
        if (!extract_sid(line, &new_sid)) {
            send_status(state, timestamp_ms, "invalid_sid");
            return;
        }
        close_active_note(state, timestamp_ms, "restart");
        state->sid = new_sid;
        state->stream_enabled = true;
        state->unknown_since_ms = 0;
        reset_poly_state(state);
        send_status(state, timestamp_ms, "started");
    } else if (strcmp(command, "stop") == 0) {
        close_active_note(state, timestamp_ms, "stop");
        state->stream_enabled = false;
        reset_poly_state(state);
        send_status(state, timestamp_ms, "stopped");
    } else if (strcmp(command, "stream_on") == 0) {
        state->stream_enabled = true;
        state->unknown_since_ms = 0;
        reset_poly_state(state);
        send_status(state, timestamp_ms, "stream_on");
    } else if (strcmp(command, "stream_off") == 0) {
        close_active_note(state, timestamp_ms, "stream_off");
        state->stream_enabled = false;
        reset_poly_state(state);
        send_status(state, timestamp_ms, "stream_off");
    } else {
        ++state->invalid_commands;
        send_status(state, timestamp_ms, "unknown_command");
    }
}

static void log_rx_sample(music_link_state_t *state)
{
    if (state->rx_sample_logged || state->rx_sample_length == 0) return;

    char hex[MUSIC_LINK_RX_SAMPLE_SIZE * 3 + 1];
    char ascii[MUSIC_LINK_RX_SAMPLE_SIZE + 1];
    size_t hex_used = 0;
    for (size_t i = 0; i < state->rx_sample_length; ++i) {
        const unsigned byte = state->rx_sample[i];
        const int written = snprintf(hex + hex_used, sizeof(hex) - hex_used,
                                     i == 0 ? "%02X" : " %02X", byte);
        if (written < 0 || (size_t)written >= sizeof(hex) - hex_used) break;
        hex_used += (size_t)written;
        ascii[i] = isprint((int)byte) ? (char)byte : '.';
    }
    hex[hex_used] = '\0';
    ascii[state->rx_sample_length] = '\0';
    ESP_LOGW(TAG, "raw RX sample (%u bytes): %s |%s|",
             (unsigned)state->rx_sample_length, hex, ascii);
    ESP_LOGW(TAG, "expected poll bytes: 7B 22 63 6D 64 22 3A 22 70 6F 6C 6C 22 7D 0A");
    state->rx_sample_logged = true;
}

static void receive_commands(music_link_state_t *state)
{
    uint8_t input[64];
    const int received = uart_read_bytes(BOARD_MUSIC_LINK_UART_PORT, input, sizeof(input), 0);
    if (received > 0) {
        state->rx_bytes += (uint32_t)received;
        const size_t remaining = sizeof(state->rx_sample) - state->rx_sample_length;
        const size_t copy_length = (size_t)received < remaining
                                       ? (size_t)received
                                       : remaining;
        if (copy_length > 0) {
            memcpy(state->rx_sample + state->rx_sample_length, input, copy_length);
            state->rx_sample_length += copy_length;
        }
    }
    for (int i = 0; i < received; ++i) {
        const char character = (char)input[i];
        if (character == '\n') {
            if (!state->rx_overflow && state->rx_length > 0) {
                state->rx_line[state->rx_length] = '\0';
                process_command(state, state->rx_line);
            } else if (state->rx_overflow) {
                ++state->invalid_commands;
                send_status(state, now_ms(), "command_too_long");
            }
            state->rx_length = 0;
            state->rx_overflow = false;
        } else if (character != '\r' && !state->rx_overflow) {
            if (state->rx_length + 1 < sizeof(state->rx_line)) {
                state->rx_line[state->rx_length++] = character;
            } else {
                state->rx_overflow = true;
            }
        }
    }
}

static void music_link_tx_task(void *argument)
{
    (void)argument;
    music_link_state_t state = {
        .stream_enabled = true,
        .active_midi = -1,
        .last_heartbeat_ms = now_ms(),
    };
    while (true) {
        receive_commands(&state);

        link_state_event_t state_event;
        bool did_work = false;
        while (xQueueReceive(s_state_queue, &state_event, 0) == pdTRUE) {
            did_work = true;
            if (state_event.type == LINK_STATE_READY) {
                state.ready = state_event.data.ready;
                if (state.ready) send_hello(&state, now_ms());
                send_status(&state, now_ms(), state.ready ? "ready" : "audio_error");
            } else {
                process_result(&state, &state_event.data.result);
            }
        }

        link_pitch_event_t pitch;
        if (xQueueReceive(s_pitch_queue, &pitch, 0) == pdTRUE) {
            did_work = true;
            if (pitch.timestamp_ms - state.last_pitch_ms >= MUSIC_UART_PITCH_INTERVAL_MS) {
                state.last_pitch_ms = pitch.timestamp_ms;
                send_pitch(&state, &pitch);
            }
        }

        const uint32_t timestamp_ms = now_ms();
        if (timestamp_ms - state.last_heartbeat_ms >= MUSIC_UART_HEARTBEAT_INTERVAL_MS) {
            state.last_heartbeat_ms = timestamp_ms;
            send_heartbeat(&state, timestamp_ms);
            did_work = true;
        }
        if (timestamp_ms - state.last_link_diag_ms >= MUSIC_LINK_DIAG_INTERVAL_MS) {
            state.last_link_diag_ms = timestamp_ms;
            const unsigned pending = s_output_queue == NULL
                                         ? 0U
                                         : (unsigned)uxQueueMessagesWaiting(s_output_queue);
            ESP_LOGI(TAG, "link diag: rx_bytes=%" PRIu32 " polls=%" PRIu32
                          " reply_frames=%" PRIu32 " invalid_cmd=%" PRIu32
                          " pending=%u tx_drop=%" PRIu32,
                     state.rx_bytes, state.poll_count, state.response_frames,
                     state.invalid_commands, pending, read_drop());
            if (state.rx_bytes == 0) {
                ESP_LOGW(TAG, "no P4 UART bytes: check P4 GPIO0/J6-4 -> S3 GPIO2/H7-8 and GND");
            } else if (state.poll_count == 0) {
                log_rx_sample(&state);
                ESP_LOGW(TAG, "UART bytes received but no valid poll: check %d 8N1 and TX/RX direction",
                         BOARD_MUSIC_LINK_BAUD_RATE);
            }
        }
        if (!did_work) vTaskDelay(pdMS_TO_TICKS(1));
    }
}

esp_err_t music_uart_link_init(void)
{
    ESP_RETURN_ON_FALSE(s_state_queue == NULL && s_pitch_queue == NULL &&
                        s_output_queue == NULL,
                        ESP_ERR_INVALID_STATE, TAG, "music link already initialized");
    s_state_queue = xQueueCreate(MUSIC_UART_STATE_QUEUE_LENGTH, sizeof(link_state_event_t));
    s_pitch_queue = xQueueCreate(MUSIC_UART_PITCH_QUEUE_LENGTH, sizeof(link_pitch_event_t));
    s_output_queue = xQueueCreate(MUSIC_LINK_OUTPUT_QUEUE_LENGTH,
                                  sizeof(link_output_frame_t));
    if (s_state_queue == NULL || s_pitch_queue == NULL || s_output_queue == NULL) {
        if (s_state_queue != NULL) vQueueDelete(s_state_queue);
        if (s_pitch_queue != NULL) vQueueDelete(s_pitch_queue);
        if (s_output_queue != NULL) vQueueDelete(s_output_queue);
        s_state_queue = NULL;
        s_pitch_queue = NULL;
        s_output_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    const uart_config_t uart_config = {
        .baud_rate = BOARD_MUSIC_LINK_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {0},
    };
    esp_err_t error = uart_param_config(BOARD_MUSIC_LINK_UART_PORT, &uart_config);
    if (error == ESP_OK) {
        error = uart_set_pin(BOARD_MUSIC_LINK_UART_PORT, BOARD_MUSIC_LINK_TX_GPIO,
                             BOARD_MUSIC_LINK_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (error == ESP_OK) {
        error = uart_driver_install(BOARD_MUSIC_LINK_UART_PORT, MUSIC_UART_RX_BUFFER_SIZE,
                                    MUSIC_UART_TX_BUFFER_SIZE, 0, NULL, 0);
    }
    if (error != ESP_OK) {
        vQueueDelete(s_state_queue);
        vQueueDelete(s_pitch_queue);
        vQueueDelete(s_output_queue);
        s_state_queue = NULL;
        s_pitch_queue = NULL;
        s_output_queue = NULL;
        return error;
    }
    uart_flush_input(BOARD_MUSIC_LINK_UART_PORT);
    const BaseType_t created = xTaskCreatePinnedToCore(
        music_link_tx_task, "MusicLinkTxTask", MUSIC_UART_TASK_STACK_SIZE, NULL,
        MUSIC_UART_TASK_PRIORITY, NULL, MUSIC_UART_TASK_CORE);
    if (created != pdPASS) {
        uart_driver_delete(BOARD_MUSIC_LINK_UART_PORT);
        vQueueDelete(s_state_queue);
        vQueueDelete(s_pitch_queue);
        vQueueDelete(s_output_queue);
        s_state_queue = NULL;
        s_pitch_queue = NULL;
        s_output_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    uint32_t actual_baud = 0;
    const esp_err_t baud_error = uart_get_baudrate(BOARD_MUSIC_LINK_UART_PORT,
                                                   &actual_baud);
    ESP_LOGI(TAG, "poll-response UART%d TX=GPIO%d RX=GPIO%d requested=%d actual=%" PRIu32 " baud",
             (int)BOARD_MUSIC_LINK_UART_PORT, (int)BOARD_MUSIC_LINK_TX_GPIO,
             (int)BOARD_MUSIC_LINK_RX_GPIO, BOARD_MUSIC_LINK_BAUD_RATE,
             baud_error == ESP_OK ? actual_baud : 0);
    return ESP_OK;
}

void music_uart_link_set_ready(bool ready)
{
    if (s_state_queue == NULL) return;
    const link_state_event_t event = {
        .type = LINK_STATE_READY,
        .data.ready = ready,
    };
    if (xQueueSend(s_state_queue, &event, 0) != pdTRUE) increment_drop();
}

void music_uart_link_submit_result(const music_result_t *result)
{
    if (s_state_queue == NULL || result == NULL) return;
    const link_state_event_t event = {
        .type = LINK_STATE_RESULT,
        .data.result = *result,
    };
    if (xQueueSend(s_state_queue, &event, 0) != pdTRUE) increment_drop();
}

void music_uart_link_submit_pitch(const music_result_t *result)
{
    if (s_pitch_queue == NULL || result == NULL || result->midi < 0 ||
        result->frequency_hz <= 0.0f || !isfinite(result->frequency_hz)) return;
    const link_pitch_event_t event = {
        .timestamp_ms = result->timestamp_ms,
        .midi = result->midi,
        .frequency_hz = result->frequency_hz,
        .cents = result->cents,
        .confidence = result->yin_confidence,
        .rms = result->rms,
    };
    if (xQueueSend(s_pitch_queue, &event, 0) != pdTRUE) increment_drop();
}
