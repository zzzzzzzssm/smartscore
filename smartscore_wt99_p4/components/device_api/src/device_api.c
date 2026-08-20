#include "device_api.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "board_sdcard.h"
#include "cJSON.h"
#include "compact_score.h"
#include "compact_score_store.h"
#include "dashscope_omr.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_source_manager.h"
#include "network_provisioning.h"
#include "practice_advice_service.h"
#include "score_capture.h"
#include "score_data.h"
#include "score_json_parser.h"
#include "score_storage.h"
#include "scoring_service.h"
#include "screen_adapter.h"
#include "s3_devices.h"
#include "speaker_service.h"
#include "sdkconfig.h"
#include "usb_midi.h"

#define DEVICE_API_MAX_BODY_BYTES 512
#define DEVICE_API_MAX_QUERY_BYTES 768
#define DEVICE_API_MAX_SEARCH_BYTES 64
#define DEVICE_API_AUDIO_PAGE_SIZE 10
#define DEVICE_API_SCORE_PAGE_SIZE 20
#define DEVICE_API_SCORE_MAX_BODY_BYTES (256 * 1024)
#define DEVICE_API_SCORING_TIMEOUT_MS 10000
#define DEVICE_API_MAX_OPEN_SOCKETS 5
#define DEVICE_API_AUDIO_UPLOAD_CHUNK_BYTES (256 * 1024)
#define DEVICE_API_AUDIO_UPLOAD_MAX_BYTES (64 * 1024 * 1024)
#define DEVICE_API_PHOTO_PAGE_SIZE 6
#define DEVICE_API_SCORE_RESULT_CHUNK_BYTES 4096

static const char *TAG = "DEVICE_API";
static httpd_handle_t s_server;

static int hex_digit_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static esp_err_t decode_query_value(const char *encoded,
                                    char *decoded,
                                    size_t decoded_capacity)
{
    if (encoded == NULL || decoded == NULL || decoded_capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t output = 0;
    for (size_t input = 0; encoded[input] != '\0'; ++input) {
        unsigned char value = (unsigned char)encoded[input];
        if (value == '%') {
            int high = hex_digit_value(encoded[input + 1]);
            int low = encoded[input + 1] == '\0'
                          ? -1
                          : hex_digit_value(encoded[input + 2]);
            if (high < 0 || low < 0) {
                return ESP_ERR_INVALID_ARG;
            }
            value = (unsigned char)((high << 4) | low);
            input += 2;
        } else if (value == '+') {
            value = ' ';
        }
        if (value == '\0' || output + 1 >= decoded_capacity) {
            return ESP_ERR_INVALID_SIZE;
        }
        decoded[output++] = (char)value;
    }
    decoded[output] = '\0';
    return ESP_OK;
}

static esp_err_t query_size_value(const char *query,
                                  const char *key,
                                  size_t default_value,
                                  size_t maximum,
                                  size_t *out_value)
{
    char value[16];
    if (query == NULL || query[0] == '\0' ||
        httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) {
        *out_value = default_value;
        return ESP_OK;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (value[0] == '\0' || end == NULL || *end != '\0' || parsed == 0 ||
        parsed > maximum) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_value = (size_t)parsed;
    return ESP_OK;
}

static const char *speaker_state_name(speaker_state_t state)
{
    switch (state) {
        case SPEAKER_STATE_UNINITIALIZED:
            return "uninitialized";
        case SPEAKER_STATE_STOPPED:
            return "stopped";
        case SPEAKER_STATE_TONE:
            return "tone";
        case SPEAKER_STATE_METRONOME:
            return "metronome";
        case SPEAKER_STATE_FILE:
            return "file";
        case SPEAKER_STATE_FILE_PAUSED:
            return "file_paused";
        case SPEAKER_STATE_STREAM:
            return "stream";
        case SPEAKER_STATE_ERROR:
            return "error";
        default:
            return "unknown";
    }
}

static void add_audio_status(cJSON *parent)
{
    speaker_status_t status;
    memset(&status, 0, sizeof(status));
    speaker_service_get_status(&status);

    cJSON *audio = cJSON_AddObjectToObject(parent, "audio");
    if (audio == NULL) {
        return;
    }
    cJSON_AddBoolToObject(audio, "ready", speaker_service_is_ready());
    cJSON_AddBoolToObject(audio, "hardware_output_enabled",
                          status.hardware_output_enabled);
    cJSON_AddBoolToObject(audio, "control_only",
                          speaker_service_is_ready() &&
                          !status.hardware_output_enabled);
    cJSON_AddStringToObject(audio, "state", speaker_state_name(status.state));
    cJSON_AddNumberToObject(audio, "frequency_hz", status.frequency_hz);
    cJSON_AddNumberToObject(audio, "duration_ms", status.duration_ms);
    cJSON_AddNumberToObject(audio, "elapsed_ms", status.elapsed_ms);
    cJSON_AddBoolToObject(audio, "sd_present", status.sd_present);
    cJSON_AddNumberToObject(audio, "sd_capacity_bytes",
                            (double)status.sd_capacity_bytes);
    cJSON_AddBoolToObject(audio, "codec_ready", status.hardware.codec_ready);
    cJSON_AddBoolToObject(audio, "amp_enabled", status.hardware.amp_enabled);
    cJSON_AddBoolToObject(audio, "muted", status.hardware.muted);
    cJSON_AddNumberToObject(audio, "volume", status.hardware.volume_percent);
    cJSON_AddNumberToObject(audio, "sample_rate_hz",
                            status.hardware.sample_rate_hz);
    cJSON_AddNumberToObject(audio, "write_errors", status.hardware.write_errors);
    cJSON_AddNumberToObject(audio, "task_stack_min_words",
                            status.task_stack_min_words);
    cJSON *hardware = cJSON_AddObjectToObject(audio, "hardware");
    if (hardware != NULL) {
        cJSON_AddBoolToObject(hardware, "output_enabled",
                              status.hardware_output_enabled);
        cJSON_AddBoolToObject(hardware, "codec_ready",
                              status.hardware.codec_ready);
        cJSON_AddBoolToObject(hardware, "amp_enabled",
                              status.hardware.amp_enabled);
        cJSON_AddBoolToObject(hardware, "muted", status.hardware.muted);
        cJSON_AddNumberToObject(hardware, "volume_percent",
                                status.hardware.volume_percent);
        cJSON_AddNumberToObject(hardware, "sample_rate_hz",
                                status.hardware.sample_rate_hz);
        cJSON_AddNumberToObject(hardware, "write_errors",
                                status.hardware.write_errors);
    }
    cJSON *metronome = cJSON_AddObjectToObject(audio, "metronome");
    if (metronome != NULL) {
        cJSON_AddBoolToObject(
            metronome, "running",
            status.metronome.state == SPEAKER_METRONOME_RUNNING);
        cJSON_AddBoolToObject(
            metronome, "paused",
            status.metronome.state == SPEAKER_METRONOME_PAUSED);
        cJSON_AddNumberToObject(metronome, "bpm", status.metronome.bpm);
        cJSON_AddNumberToObject(metronome, "beats_per_measure",
                                status.metronome.beats_per_measure);
        cJSON_AddNumberToObject(metronome, "beat_unit",
                                status.metronome.beat_unit);
        cJSON_AddNumberToObject(metronome, "beat_index",
                                status.metronome.beat_index);
        cJSON_AddNumberToObject(metronome, "queue_errors",
                                status.metronome.queue_errors);
    }
    cJSON *file = cJSON_AddObjectToObject(audio, "file");
    if (file != NULL) {
        cJSON_AddStringToObject(file, "name", status.file_name);
        cJSON_AddBoolToObject(file, "paused",
                              status.state == SPEAKER_STATE_FILE_PAUSED);
    }
    esp_err_t last_error = status.last_error != ESP_OK
                               ? status.last_error
                               : status.hardware.last_error;
    cJSON_AddStringToObject(audio, "last_error", esp_err_to_name(last_error));
}

static const char *preparation_phase_name(screen_preparation_phase_t phase)
{
    switch (phase) {
    case SCREEN_PREPARATION_PREPARED: return "prepared";
    case SCREEN_PREPARATION_STARTING: return "starting";
    case SCREEN_PREPARATION_READING: return "reading";
    case SCREEN_PREPARATION_FOLLOWING: return "following";
    case SCREEN_PREPARATION_ERROR: return "error";
    case SCREEN_PREPARATION_IDLE:
    default: return "idle";
    }
}

static void add_preparation_status(cJSON *parent)
{
    screen_preparation_status_t status = {0};
    screen_adapter_get_preparation_status(&status);
    cJSON *preparation =
        cJSON_AddObjectToObject(parent, "preparation");
    if (!preparation) return;
    cJSON_AddBoolToObject(preparation, "valid", status.valid);
    cJSON_AddBoolToObject(preparation, "screen_ready",
                          status.screen_ready);
    cJSON_AddNumberToObject(preparation, "revision", status.revision);
    cJSON_AddStringToObject(preparation, "phase",
                            preparation_phase_name(status.phase));
    cJSON_AddStringToObject(
        preparation, "notation_type",
        status.notation == SCREEN_PREPARATION_NUMBERED
            ? "numbered" : "staff");
    cJSON_AddStringToObject(
        preparation, "mode",
        status.mode == SCREEN_PREPARATION_READ_ONLY
            ? "read_only" : "follow");
    cJSON_AddStringToObject(
        preparation, "input_source",
        status.input == SCREEN_PREPARATION_USB_MIDI
            ? "usb_midi" : "audio_s3");
    cJSON_AddStringToObject(preparation, "filename", status.filename);
    cJSON_AddStringToObject(preparation, "title", status.title);
    cJSON_AddStringToObject(preparation, "key", status.key);
    cJSON_AddNumberToObject(preparation, "bpm", status.bpm);
    cJSON_AddNumberToObject(preparation, "time_sig_num",
                            status.time_sig_num);
    cJSON_AddNumberToObject(preparation, "time_sig_den",
                            status.time_sig_den);
    cJSON_AddNumberToObject(preparation, "note_count",
                            status.note_count);
    cJSON_AddStringToObject(preparation, "last_error",
                            esp_err_to_name(status.last_error));
    cJSON_AddStringToObject(preparation, "message", status.message);
}

static esp_err_t send_json(httpd_req_t *request,
                           const char *http_status,
                           cJSON *root)
{
    if (root == NULL) {
        return httpd_resp_send_err(request,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "out of memory");
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return httpd_resp_send_err(request,
                                   HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "out of memory");
    }
    httpd_resp_set_status(request, http_status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(request, json);
    cJSON_free(json);
    return err;
}

static esp_err_t send_error_json(httpd_req_t *request,
                                 const char *http_status,
                                 const char *code,
                                 const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", code);
        cJSON_AddStringToObject(root, "message", message);
    }
    return send_json(request, http_status, root);
}

static esp_err_t send_command_result(httpd_req_t *request,
                                     esp_err_t command_error,
                                     const char *message)
{
    if (command_error != ESP_OK) {
        const char *http_status = command_error == ESP_ERR_INVALID_ARG
                                      ? "400 Bad Request"
                                      : "409 Conflict";
        return send_error_json(request,
                               http_status,
                               esp_err_to_name(command_error),
                               message);
    }
    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddBoolToObject(root, "queued", true);
        cJSON_AddStringToObject(root, "message", message);
    }
    return send_json(request, "202 Accepted", root);
}

static esp_err_t receive_json(httpd_req_t *request, cJSON **out_root)
{
    if (request == NULL || out_root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_root = NULL;
    if (request->content_len == 0 ||
        request->content_len > DEVICE_API_MAX_BODY_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    char body[DEVICE_API_MAX_BODY_BYTES + 1];
    size_t received = 0;
    while (received < request->content_len) {
        int count = httpd_req_recv(request,
                                   body + received,
                                   request->content_len - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (count <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)count;
    }
    body[received] = '\0';
    *out_root = cJSON_ParseWithLength(body, received);
    return *out_root != NULL && cJSON_IsObject(*out_root)
               ? ESP_OK
               : ESP_ERR_INVALID_ARG;
}

static esp_err_t receive_large_body(httpd_req_t *request,
                                    size_t maximum,
                                    char **out_body,
                                    size_t *out_length)
{
    if (request == NULL || out_body == NULL || out_length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_body = NULL;
    *out_length = 0;
    if (request->content_len == 0 || request->content_len > maximum) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t allocation_size = request->content_len + 1U;
    char *body = NULL;
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        body = heap_caps_malloc(allocation_size,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (body == NULL) {
        body = heap_caps_malloc(allocation_size,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t received = 0;
    while (received < request->content_len) {
        int count = httpd_req_recv(request, body + received,
                                   request->content_len - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (count <= 0) {
            heap_caps_free(body);
            return ESP_FAIL;
        }
        received += (size_t)count;
    }
    body[received] = '\0';
    *out_body = body;
    *out_length = received;
    return ESP_OK;
}

static esp_err_t receive_optional_json(httpd_req_t *request, cJSON **out_root)
{
    if (request == NULL || out_root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->content_len == 0) {
        *out_root = cJSON_CreateObject();
        return *out_root != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    }
    return receive_json(request, out_root);
}

static esp_err_t ping_handler(httpd_req_t *request)
{
    network_status_t network = network_provisioning_get_status();
    bool connected = network.state == NETWORK_STATE_WIFI_CONNECTED;
    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddBoolToObject(root, "ok", connected);
        cJSON_AddStringToObject(root, "device", "SmartScore-WT99");
        cJSON_AddStringToObject(root, "network",
                                connected ? "connected" : "disconnected");
        cJSON_AddStringToObject(root, "ip", network.ip);
    }
    return send_json(request, "200 OK", root);
}

static void add_input_status(cJSON *root,
                             const input_source_status_t *status)
{
    cJSON_AddStringToObject(root, "selected_input",
                            input_source_name(status->selected_input));
    cJSON_AddStringToObject(root, "active_input",
                            input_source_name(status->active_input));
    cJSON_AddBoolToObject(root, "input_locked", status->input_locked);
    cJSON_AddBoolToObject(root, "usb_midi_connected",
                          status->usb_midi_connected);
    cJSON_AddNumberToObject(root, "vid", status->usb_midi_vid);
    cJSON_AddNumberToObject(root, "pid", status->usb_midi_pid);
    cJSON_AddStringToObject(root, "product", status->usb_midi_product);
    cJSON_AddBoolToObject(root, "audio_s3_connected",
                          status->audio_s3_connected);
    cJSON_AddBoolToObject(root, "audio_s3_implemented",
                          status->audio_s3_implemented);
}

static esp_err_t status_handler(httpd_req_t *request)
{
    network_status_t network = network_provisioning_get_status();
    bool connected = network.state == NETWORK_STATE_WIFI_CONNECTED;
    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddBoolToObject(root, "ok", connected);
        cJSON_AddStringToObject(root, "device", "SmartScore-WT99");
        cJSON_AddBoolToObject(root, "p4_ready", true);
        cJSON_AddBoolToObject(root, "c5_network_ready", connected);
        cJSON_AddStringToObject(root, "state",
                                network_provisioning_state_name(network.state));
        cJSON_AddStringToObject(root, "ip", network.ip);
        cJSON_AddNumberToObject(root, "retry_count", network.retry_count);
        cJSON_AddNumberToObject(root, "internal_heap_free",
                                heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(root, "psram_free",
                                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        input_source_status_t input;
        input_source_manager_get_status(&input);
        add_input_status(root, &input);
        cJSON_AddStringToObject(root, "active_source",
                                input_source_name(input.active_input));
        usb_midi_status_t midi;
        memset(&midi, 0, sizeof(midi));
        usb_midi_get_status(&midi);
        cJSON_AddNumberToObject(root, "usb_midi_dropped_events",
                                midi.dropped_events);
        if (midi.error[0] != '\0') {
            cJSON_AddStringToObject(root, "usb_midi_error", midi.error);
        }
        score_data_status_t score;
        score_data_get_status(&score);
        scoring_service_status_t practice;
        scoring_service_get_status(&practice);
        cJSON_AddNumberToObject(root, "target_count", score.note_count);
        cJSON_AddNumberToObject(root, "played_count", practice.played_count);
        cJSON_AddStringToObject(
            root, "practice_state",
            scoring_service_state_name(practice.state));
        cJSON_AddStringToObject(root, "input_source",
                                input_source_name(input.active_input));
        cJSON_AddStringToObject(root, "scoring_profile", practice.profile);
        practice_advice_status_t advice;
        practice_advice_service_get_status(&advice);
        cJSON_AddStringToObject(root, "practice_session_id",
                                advice.session_id);
        cJSON_AddStringToObject(root, "advice_state",
                                practice_advice_state_name(advice.state));
        cJSON_AddBoolToObject(root, "advice_ready",
                              advice.state == PRACTICE_ADVICE_READY);
        cJSON_AddStringToObject(root, "advice_message", advice.message);
        if (advice.error[0] != '\0') {
            cJSON_AddStringToObject(root, "advice_error", advice.error);
        }
        if (practice.error[0] != '\0') {
            cJSON_AddStringToObject(root, "practice_error", practice.error);
            cJSON_AddStringToObject(root, "practice_message",
                                    practice.message);
        }
        if (network.failure_reason[0] != '\0') {
            cJSON_AddStringToObject(root, "reason", network.failure_reason);
        }
        add_audio_status(root);
    }
    return send_json(request, "200 OK", root);
}

static esp_err_t input_status_handler(httpd_req_t *request)
{
    input_source_status_t status;
    input_source_manager_get_status(&status);
    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddBoolToObject(root, "ok", true);
        add_input_status(root, &status);
    }
    return send_json(request, "200 OK", root);
}

static esp_err_t input_select_handler(httpd_req_t *request)
{
    input_source_status_t current;
    input_source_manager_get_status(&current);
    if (current.input_locked) {
        return send_error_json(request, "409 Conflict", "practice_running",
                               "input source is locked during practice");
    }

    cJSON *root = NULL;
    esp_err_t err = receive_json(request, &root);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request",
                               "invalid_input_request",
                               "body must be a JSON object");
    }
    cJSON *source_json = cJSON_GetObjectItemCaseSensitive(root, "source");
    input_source_t source = INPUT_SOURCE_NONE;
    bool valid = cJSON_IsString(source_json) &&
                 input_source_from_name(source_json->valuestring, &source) &&
                 (source == INPUT_SOURCE_USB_MIDI ||
                  source == INPUT_SOURCE_AUDIO_S3);
    cJSON_Delete(root);
    if (!valid) {
        return send_error_json(
            request, "400 Bad Request", "invalid_input_source",
            "source must be usb_midi or audio_s3");
    }

    err = input_source_manager_select(source);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error_json(request, "409 Conflict", "practice_running",
                               "input source is locked during practice");
    }
    if (err != ESP_OK) {
        return send_error_json(request, "500 Internal Server Error",
                               "input_selection_save_failed",
                               "unable to save selected input to NVS");
    }

    input_source_status_t updated;
    input_source_manager_get_status(&updated);
    cJSON *response = cJSON_CreateObject();
    if (response != NULL) {
        cJSON_AddBoolToObject(response, "ok", true);
        add_input_status(response, &updated);
    }
    return send_json(request, "200 OK", response);
}

static esp_err_t audio_status_handler(httpd_req_t *request)
{
    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddBoolToObject(root, "ok", speaker_service_is_ready());
        add_audio_status(root);
    }
    return send_json(request, "200 OK", root);
}

static esp_err_t score_upload_handler(httpd_req_t *request)
{
    char *body = NULL;
    size_t body_length = 0;
    esp_err_t err = receive_large_body(request,
                                        DEVICE_API_SCORE_MAX_BODY_BYTES,
                                        &body, &body_length);
    if (err != ESP_OK) {
        const char *status = err == ESP_ERR_INVALID_SIZE
                                 ? "413 Payload Too Large"
                                 : "500 Internal Server Error";
        const char *code = err == ESP_ERR_INVALID_SIZE
                               ? "score_json_too_large"
                               : "score_body_read_failed";
        return send_error_json(request, status, code,
                               "failed to read score JSON");
    }

    char parse_error[96] = {0};
    size_t note_count = 0;
    err = score_json_parse_and_store(body, body_length,
                                     parse_error, sizeof(parse_error),
                                     &note_count);
    if (err != ESP_OK) {
        heap_caps_free(body);
        const char *status = err == ESP_ERR_INVALID_SIZE
                                 ? "413 Payload Too Large"
                                 : err == ESP_ERR_NO_MEM
                                       ? "500 Internal Server Error"
                                       : "400 Bad Request";
        return send_error_json(request, status,
                               parse_error[0] != '\0'
                                   ? parse_error
                                   : esp_err_to_name(err),
                               "score JSON was not stored");
    }

    err = screen_adapter_prepare_score_json(
        body, body_length, "REMOTE.JSON");
    heap_caps_free(body);
    if (err != ESP_OK) {
        return send_error_json(
            request,
            err == ESP_ERR_INVALID_STATE
                ? "409 Conflict" : "500 Internal Server Error",
            "screen_preparation_failed",
            err == ESP_ERR_INVALID_STATE
                ? "screen is not ready or another mode is active"
                : "score was parsed but the screen preparation page failed");
    }

    score_data_status_t score;
    score_data_get_status(&score);
    input_source_status_t input;
    input_source_manager_get_status(&input);
    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "message", "score stored");
        cJSON_AddNumberToObject(root, "notes", note_count);
        cJSON_AddNumberToObject(root, "note_count", note_count);
        cJSON_AddStringToObject(root, "title", score.title);
        cJSON_AddNumberToObject(root, "bpm", score.bpm);
        cJSON_AddStringToObject(root, "input_source",
                                input_source_name(input.selected_input));
        cJSON_AddStringToObject(root, "profile", "beginner_mono_v2");
        add_preparation_status(root);
    }
    return send_json(request, "200 OK", root);
}

typedef struct {
    uint32_t event_index;
    uint32_t start_ms;
    uint16_t staff;
    uint16_t voice;
    uint8_t flags;
    size_t representative;
} compact_playback_event_group_t;

static void compact_playback_notation_links(
    const compact_score_playback_t *playback,
    uint8_t *tie_flags,
    uint8_t *slur_start,
    uint8_t *slur_stop,
    uint8_t *gliss_start,
    uint8_t *gliss_stop)
{
    if (playback == NULL || playback->notes == NULL ||
        playback->note_count == 0 || tie_flags == NULL ||
        slur_start == NULL || slur_stop == NULL ||
        gliss_start == NULL || gliss_stop == NULL) {
        return;
    }
    compact_playback_event_group_t *groups = heap_caps_calloc(
        playback->note_count, sizeof(*groups),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (groups == NULL) {
        groups = calloc(playback->note_count, sizeof(*groups));
    }
    if (groups == NULL) return;

    size_t group_count = 0;
    for (size_t index = 0; index < playback->note_count; ++index) {
        const compact_score_playback_note_t *note =
            &playback->notes[index];
        size_t group = 0;
        while (group < group_count &&
               groups[group].event_index != note->event_index) {
            ++group;
        }
        if (group == group_count) {
            groups[group] = (compact_playback_event_group_t){
                .event_index = note->event_index,
                .start_ms = note->start_ms,
                .staff = note->staff,
                .voice = note->voice,
                .flags = note->flags,
                .representative = index,
            };
            ++group_count;
        } else if (note->midi >
                   playback->notes[groups[group].representative].midi) {
            groups[group].representative = index;
        }
    }

    uint16_t next_slur_id = 1;
    uint16_t next_gliss_id = 1;
    for (size_t group = 0; group < group_count; ++group) {
        if ((groups[group].flags & 0x0bU) == 0) continue;
        size_t target = group_count;
        for (size_t candidate = group + 1; candidate < group_count;
             ++candidate) {
            if (groups[candidate].staff == groups[group].staff &&
                groups[candidate].voice == groups[group].voice &&
                (groups[candidate].start_ms > groups[group].start_ms ||
                 groups[candidate].event_index >
                     groups[group].event_index)) {
                target = candidate;
                break;
            }
        }
        if (target == group_count) continue;

        if ((groups[group].flags & 0x01U) != 0) {
            for (size_t start_index = 0;
                 start_index < playback->note_count; ++start_index) {
                const compact_score_playback_note_t *start_note =
                    &playback->notes[start_index];
                if (start_note->event_index != groups[group].event_index)
                    continue;
                for (size_t stop_index = 0;
                     stop_index < playback->note_count; ++stop_index) {
                    const compact_score_playback_note_t *stop_note =
                        &playback->notes[stop_index];
                    if (stop_note->event_index ==
                            groups[target].event_index &&
                        stop_note->midi == start_note->midi) {
                        tie_flags[start_index] |= 0x01U;
                        tie_flags[stop_index] |= 0x02U;
                        break;
                    }
                }
            }
        }

        if ((groups[group].flags & 0x02U) != 0 &&
            next_slur_id <= UINT8_MAX) {
            const size_t start_index = groups[group].representative;
            const size_t stop_index = groups[target].representative;
            slur_start[start_index] = (uint8_t)next_slur_id;
            slur_stop[stop_index] = (uint8_t)next_slur_id;
            ++next_slur_id;
        }
        if ((groups[group].flags & 0x08U) != 0 &&
            next_gliss_id <= UINT8_MAX) {
            const size_t start_index = groups[group].representative;
            const size_t stop_index = groups[target].representative;
            gliss_start[start_index] = (uint8_t)next_gliss_id;
            gliss_stop[stop_index] = (uint8_t)next_gliss_id;
            ++next_gliss_id;
        }
    }
    heap_caps_free(groups);
}

static esp_err_t ai_sheet_to_score_handler(httpd_req_t *request)
{
    if (!dashscope_omr_api_key_configured()) {
        return send_error_json(request, "503 Service Unavailable",
                               "dashscope_key_missing",
                               "DASHSCOPE_API_KEY is not configured in the firmware");
    }

    char mime_type[48] = "image/jpeg";
    if (httpd_req_get_hdr_value_str(request, "Content-Type", mime_type,
                                    sizeof(mime_type)) != ESP_OK) {
        strlcpy(mime_type, "image/jpeg", sizeof(mime_type));
    }
    char *parameters = strchr(mime_type, ';');
    if (parameters != NULL) {
        *parameters = '\0';
    }
    if (strcmp(mime_type, "image/jpeg") != 0 &&
        strcmp(mime_type, "image/jpg") != 0) {
        return send_error_json(request, "415 Unsupported Media Type",
                               "sheet_image_not_jpeg",
                               "send one complete JPEG page");
    }

    if (request->content_len == 0) {
        return send_error_json(request, "400 Bad Request",
                               "sheet_image_missing",
                               "send a sheet image in the request body");
    }
    if (request->content_len > SCORE_CAPTURE_MAX_JPEG_BYTES) {
        return send_error_json(request, "413 Payload Too Large",
                               "sheet_image_too_large",
                               "JPEG exceeds the full-page upload limit");
    }

    char *image = NULL;
    size_t image_length = 0;
    esp_err_t err = receive_large_body(request, SCORE_CAPTURE_MAX_JPEG_BYTES,
                                        &image, &image_length);
    if (err != ESP_OK) {
        return send_error_json(request, "400 Bad Request",
                               "sheet_image_read_failed",
                               "failed to read the sheet image");
    }

    char task_id[SCORE_CAPTURE_TASK_ID_CAPACITY];
    snprintf(task_id, sizeof(task_id), "omr-%08lx-%08lx-%08lx",
             (unsigned long)((uint64_t)esp_timer_get_time() & 0xffffffffU),
             (unsigned long)esp_random(), (unsigned long)esp_random());
    score_capture_t capture = {0};
    err = score_capture_from_jpeg((const uint8_t *)image, image_length,
                                  task_id, &capture);
    if (err != ESP_OK) {
        heap_caps_free(image);
        return send_error_json(request, "400 Bad Request",
                               "invalid_full_page_jpeg",
                               "JPEG header or dimensions are invalid");
    }

    dashscope_omr_result_t omr = {0};
    dashscope_omr_error_t omr_error =
        dashscope_omr_recognize(&capture, &omr);
    heap_caps_free(image);
    if (omr_error != DASHSCOPE_OMR_OK) {
        const char *status = "502 Bad Gateway";
        if (omr_error == DASHSCOPE_OMR_ERR_BUSY) {
            status = "409 Conflict";
        } else if (omr_error == DASHSCOPE_OMR_ERR_HTTP_RATE_LIMIT) {
            status = "429 Too Many Requests";
        } else if (omr_error == DASHSCOPE_OMR_ERR_TIMEOUT) {
            status = "504 Gateway Timeout";
        } else if (omr_error == DASHSCOPE_OMR_ERR_NOT_CONFIGURED) {
            status = "503 Service Unavailable";
        } else if (omr_error == DASHSCOPE_OMR_ERR_SCORE_SCHEMA ||
                   omr_error == DASHSCOPE_OMR_ERR_INNER_JSON) {
            status = "422 Unprocessable Entity";
        }
        char message[160];
        if (omr_error == DASHSCOPE_OMR_ERR_SCORE_SCHEMA) {
            snprintf(message, sizeof(message),
                     "compact score validation failed at %s: %s",
                     omr.score_error.path,
                     compact_score_error_name(omr.score_error.code));
        } else {
            snprintf(message, sizeof(message),
                     "DashScope full-page recognition failed: %s",
                     dashscope_omr_error_name(omr_error));
        }
        const char *code = dashscope_omr_error_name(omr_error);
        dashscope_omr_result_free(&omr);
        return send_error_json(request, status, code, message);
    }

    size_t event_count = omr.score->event_count;
    compact_score_kind_t kind = omr.score->kind;
    compact_score_meta_t score_meta = omr.score->meta;
    compact_score_playback_t playback = {0};
    compact_score_error_t playback_error = {0};
    compact_score_error_code_t playback_result = COMPACT_SCORE_ERR_CAPACITY;
    if (omr.score->sounding_note_count <= SCORE_DATA_MAX_NOTES) {
        playback_result = compact_score_build_playback(
            omr.score, 120, &playback, &playback_error);
    } else {
        playback_error.code = COMPACT_SCORE_ERR_CAPACITY;
        strlcpy(playback_error.path, "playback.notes",
                sizeof(playback_error.path));
    }

    if (compact_score_store_init() != ESP_OK) {
        compact_score_playback_free(&playback);
        dashscope_omr_result_free(&omr);
        return send_error_json(request, "500 Internal Server Error",
                               "compact_score_store_init_failed",
                               "unable to initialize compact score storage");
    }
    compact_score_document_t *owned_score = omr.score;
    omr.score = NULL;
    err = compact_score_store_replace(task_id, owned_score);
    if (err != ESP_OK) {
        compact_score_free(owned_score);
        compact_score_playback_free(&playback);
        dashscope_omr_result_free(&omr);
        return send_error_json(request, "409 Conflict",
                               "duplicate_omr_task",
                               "recognition task was already committed");
    }

    bool playback_ready = playback_result == COMPACT_SCORE_OK;
    bool stored_for_scoring = false;
    bool screen_prepared = false;
    char *legacy_json = NULL;
    if (playback_ready) {
        uint8_t *notation_links = heap_caps_calloc(
            playback.note_count, 5U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (notation_links == NULL) {
            notation_links = calloc(playback.note_count, 5U);
        }
        uint8_t *tie_flags = notation_links;
        uint8_t *slur_start = notation_links != NULL ?
                              notation_links + playback.note_count : NULL;
        uint8_t *slur_stop = notation_links != NULL ?
                             notation_links + playback.note_count * 2U : NULL;
        uint8_t *gliss_start = notation_links != NULL ?
                               notation_links + playback.note_count * 3U : NULL;
        uint8_t *gliss_stop = notation_links != NULL ?
                              notation_links + playback.note_count * 4U : NULL;
        compact_playback_notation_links(&playback, tie_flags,
                                        slur_start, slur_stop,
                                        gliss_start, gliss_stop);
        cJSON *legacy = cJSON_CreateObject();
        cJSON *notes = cJSON_AddArrayToObject(legacy, "notes");
        if (legacy != NULL && notes != NULL) {
            cJSON_AddStringToObject(legacy, "title", "AI full-page OMR");
            cJSON_AddNumberToObject(legacy, "bpm", playback.bpm);
            char time_signature[16] = "4/4";
            if (score_meta.has_time_signature) {
                snprintf(time_signature, sizeof(time_signature), "%u/%u",
                         score_meta.time_signature_numerator,
                         score_meta.time_signature_denominator);
            }
            cJSON_AddStringToObject(legacy, "time_signature", time_signature);
            cJSON_AddStringToObject(
                legacy, "key",
                score_meta.has_nkey ? score_meta.nkey : "unknown");
            for (size_t index = 0; index < playback.note_count; ++index) {
                const compact_score_playback_note_t *note =
                    &playback.notes[index];
                cJSON *item = cJSON_CreateObject();
                if (item == NULL) {
                    cJSON_Delete(legacy);
                    legacy = NULL;
                    break;
                }
                cJSON_AddNumberToObject(item, "midi", note->midi);
                cJSON_AddNumberToObject(item, "start",
                                        (double)note->start_ms / 1000.0);
                cJSON_AddNumberToObject(item, "duration",
                                        (double)note->duration_ms / 1000.0);
                cJSON_AddNumberToObject(item, "staff", note->staff);
                cJSON_AddNumberToObject(item, "voice", note->voice);
                cJSON_AddNumberToObject(item, "event_index",
                                        note->event_index);
                cJSON_AddNumberToObject(item, "notation_flags",
                                        note->flags);
                if (tie_flags != NULL && tie_flags[index] != 0) {
                    cJSON_AddNumberToObject(item, "tie_flags",
                                            tie_flags[index]);
                }
                if (slur_start != NULL && slur_start[index] != 0) {
                    cJSON_AddNumberToObject(item, "slur_start",
                                            slur_start[index]);
                }
                if (slur_stop != NULL && slur_stop[index] != 0) {
                    cJSON_AddNumberToObject(item, "slur_stop",
                                            slur_stop[index]);
                }
                if (gliss_start != NULL && gliss_start[index] != 0) {
                    cJSON_AddNumberToObject(item, "gliss_start",
                                            gliss_start[index]);
                }
                if (gliss_stop != NULL && gliss_stop[index] != 0) {
                    cJSON_AddNumberToObject(item, "gliss_stop",
                                            gliss_stop[index]);
                }
                cJSON_AddItemToArray(notes, item);
            }
            if (legacy != NULL) {
                legacy_json = cJSON_PrintUnformatted(legacy);
                cJSON_Delete(legacy);
            }
        } else {
            cJSON_Delete(legacy);
        }
        heap_caps_free(notation_links);
        if (legacy_json == NULL) {
            playback_ready = false;
            playback_error.code = COMPACT_SCORE_ERR_NO_MEMORY;
            strlcpy(playback_error.path, "playback.json",
                    sizeof(playback_error.path));
        }
    }

    size_t note_count = 0;
    if (playback_ready) {
        char parse_error[96] = {0};
        err = score_json_parse_and_store(
            legacy_json, strlen(legacy_json), parse_error,
            sizeof(parse_error), &note_count);
        stored_for_scoring = err == ESP_OK;
        if (stored_for_scoring) {
            esp_err_t screen_error = screen_adapter_prepare_score_json(
                legacy_json, strlen(legacy_json), "AI_OMR.JSON");
            screen_prepared = screen_error == ESP_OK;
            if (screen_error != ESP_OK) {
                ESP_LOGW(TAG,
                         "OMR stored but screen preparation failed task=%s error=%s",
                         task_id, esp_err_to_name(screen_error));
            }
        } else {
            playback_ready = false;
            playback_error.code = err == ESP_ERR_INVALID_SIZE
                                      ? COMPACT_SCORE_ERR_CAPACITY
                                      : COMPACT_SCORE_ERR_UNPLAYABLE;
            strlcpy(playback_error.path,
                    parse_error[0] != '\0' ? parse_error : "score_data",
                    sizeof(playback_error.path));
        }
    }

    cJSON *response = cJSON_CreateObject();
    cJSON *compact = cJSON_Parse(omr.compact_json);
    if (response == NULL || !cJSON_IsObject(compact)) {
        cJSON_Delete(response);
        cJSON_Delete(compact);
        cJSON_free(legacy_json);
        compact_score_playback_free(&playback);
        dashscope_omr_result_free(&omr);
        return send_error_json(request, "500 Internal Server Error",
                               "omr_response_build_failed",
                               "unable to build recognition response");
    }
    cJSON_AddBoolToObject(response, "ok", true);
    cJSON_AddStringToObject(response, "task_id", task_id);
    cJSON_AddStringToObject(response, "source", "alibaba-cloud-bailian");
    cJSON_AddStringToObject(response, "model", DASHSCOPE_OMR_MODEL);
    cJSON_AddStringToObject(response, "kind",
                            kind == COMPACT_SCORE_KIND_NUMBERED ? "n" : "s");
    cJSON_AddItemToObject(response, "compact_score", compact);
    cJSON_AddBoolToObject(response, "playback_ready", playback_ready);
    cJSON_AddBoolToObject(response, "stored_for_scoring", stored_for_scoring);
    cJSON_AddBoolToObject(response, "screen_prepared", screen_prepared);
    cJSON_AddNumberToObject(response, "event_count", event_count);
    cJSON_AddNumberToObject(response, "note_count", note_count);
    cJSON_AddNumberToObject(response, "image_bytes", image_length);
    cJSON_AddNumberToObject(response, "image_width", capture.width);
    cJSON_AddNumberToObject(response, "image_height", capture.height);
    cJSON_AddBoolToObject(response, "model_downscaled",
                          capture.model_will_downscale);
    cJSON_AddNumberToObject(response, "request_elapsed_ms", omr.elapsed_ms);
    cJSON_AddNumberToObject(response, "http_status", omr.http_status);
    cJSON_AddNumberToObject(response, "attempt_count", omr.attempt_count);
    cJSON *usage = cJSON_AddObjectToObject(response, "usage");
    if (usage != NULL) {
        cJSON_AddNumberToObject(usage, "input_tokens",
                                omr.usage.input_tokens);
        cJSON_AddNumberToObject(usage, "output_tokens",
                                omr.usage.output_tokens);
        cJSON_AddNumberToObject(usage, "total_tokens",
                                omr.usage.total_tokens);
    }
    cJSON *notes_response = cJSON_AddArrayToObject(response, "notes");
    if (notes_response != NULL && legacy_json != NULL) {
        cJSON *legacy = cJSON_Parse(legacy_json);
        cJSON *legacy_notes = cJSON_IsObject(legacy)
                                  ? cJSON_DetachItemFromObject(legacy, "notes")
                                  : NULL;
        if (cJSON_IsArray(legacy_notes)) {
            cJSON_ReplaceItemInObject(response, "notes", legacy_notes);
        } else {
            cJSON_Delete(legacy_notes);
        }
        cJSON_Delete(legacy);
    }
    if (playback_ready) {
        cJSON_AddStringToObject(response, "title", "AI full-page OMR");
        cJSON_AddNumberToObject(response, "bpm", playback.bpm);
    } else {
        cJSON_AddStringToObject(response, "playback_error",
                                compact_score_error_name(playback_error.code));
        cJSON_AddStringToObject(response, "playback_error_path",
                                playback_error.path);
    }

    cJSON_free(legacy_json);
    compact_score_playback_free(&playback);
    dashscope_omr_result_free(&omr);
    return send_json(request, "200 OK", response);
}

static cJSON *build_advice_status_json(
    const practice_advice_status_t *status,
    bool include_advice)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "session_id", status->session_id);
    cJSON_AddStringToObject(root, "state",
                            practice_advice_state_name(status->state));
    cJSON_AddBoolToObject(root, "ready",
                          status->state == PRACTICE_ADVICE_READY);
    cJSON_AddStringToObject(root, "message", status->message);
    if (status->error[0] != '\0') {
        cJSON_AddStringToObject(root, "error_code", status->error);
    }
    if (include_advice && status->state == PRACTICE_ADVICE_READY) {
        char *advice_json = NULL;
        if (practice_advice_service_copy_advice(&advice_json, NULL) != ESP_OK) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON *advice = cJSON_Parse(advice_json);
        free(advice_json);
        if (!cJSON_IsObject(advice)) {
            cJSON_Delete(advice);
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToObject(root, "advice", advice);
    }
    return root;
}

static esp_err_t practice_advice_handler(httpd_req_t *request)
{
    practice_advice_status_t status;
    practice_advice_service_get_status(&status);
    cJSON *root = build_advice_status_json(&status, true);
    if (root == NULL) {
        return send_error_json(request, "500 Internal Server Error",
                               "advice_response_build_failed",
                               "unable to build practice advice response");
    }
    return send_json(request, "200 OK", root);
}

static esp_err_t ai_score_handler(httpd_req_t *request)
{
    if (request->content_len > 0) {
        cJSON *body = NULL;
        esp_err_t body_err = receive_json(request, &body);
        cJSON_Delete(body);
        if (body_err != ESP_OK) {
            return send_error_json(request, "400 Bad Request",
                                   "invalid_ai_score_request",
                                   "body must be an optional JSON object");
        }
    }
    practice_advice_status_t status;
    practice_advice_service_get_status(&status);
    if (status.state == PRACTICE_ADVICE_READY) {
        char *advice_json = NULL;
        if (practice_advice_service_copy_advice(&advice_json, NULL) != ESP_OK) {
            return send_error_json(request, "500 Internal Server Error",
                                   "advice_copy_failed",
                                   "unable to copy practice advice");
        }
        cJSON *advice = cJSON_Parse(advice_json);
        free(advice_json);
        if (!cJSON_IsObject(advice)) {
            cJSON_Delete(advice);
            return send_error_json(request, "502 Bad Gateway",
                                   "invalid_advice_json",
                                   "stored practice advice is invalid");
        }
        return send_json(request, "200 OK", advice);
    }
    if (status.state == PRACTICE_ADVICE_WAITING_SCORE ||
        status.state == PRACTICE_ADVICE_RUNNING) {
        cJSON *root = build_advice_status_json(&status, false);
        if (root == NULL) {
            return send_error_json(request, "500 Internal Server Error",
                                   "advice_response_build_failed",
                                   "unable to build practice advice response");
        }
        return send_json(request, "202 Accepted", root);
    }
    if (status.state == PRACTICE_ADVICE_SKIPPED_OFFLINE) {
        return send_error_json(request, "409 Conflict",
                               "advice_skipped_offline", status.message);
    }
    if (status.state == PRACTICE_ADVICE_FAILED) {
        const bool key_missing =
            strcmp(status.error, "deepseek_key_missing") == 0;
        return send_error_json(request,
                               key_missing ? "503 Service Unavailable"
                                           : "502 Bad Gateway",
                               status.error[0] != '\0'
                                   ? status.error : "advice_failed",
                               status.message);
    }
    return send_error_json(request, "409 Conflict",
                           "advice_not_available",
                           "complete a practice session before viewing advice");
}

static bool contains_search_text(const char *text, const char *search)
{
    if (!search || search[0] == '\0') return true;
    if (!text) return false;

    size_t search_length = strlen(search);
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0'; ++cursor) {
        size_t index = 0;
        while (index < search_length && cursor[index] != '\0') {
            unsigned char left = cursor[index];
            unsigned char right = (unsigned char)search[index];
            if (left >= 'A' && left <= 'Z') left = (unsigned char)(left + 32);
            if (right >= 'A' && right <= 'Z') right = (unsigned char)(right + 32);
            if (left != right) break;
            ++index;
        }
        if (index == search_length) return true;
    }
    return false;
}

static esp_err_t read_query(httpd_req_t *request, char *query,
                            size_t query_capacity)
{
    size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length + 1 > query_capacity) return ESP_ERR_INVALID_SIZE;
    if (query_length > 0 &&
        httpd_req_get_url_query_str(request, query, query_capacity) != ESP_OK)
        return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static esp_err_t query_text_value(const char *query, const char *key,
                                  char *value, size_t value_capacity)
{
    value[0] = '\0';
    if (!query || query[0] == '\0') return ESP_ERR_NOT_FOUND;

    char encoded[DEVICE_API_MAX_QUERY_BYTES + 1] = {0};
    if (httpd_query_key_value(query, key, encoded, sizeof(encoded)) != ESP_OK)
        return ESP_ERR_NOT_FOUND;
    return decode_query_value(encoded, value, value_capacity);
}

static esp_err_t sd_scores_handler(httpd_req_t *request)
{
    char query[DEVICE_API_MAX_QUERY_BYTES + 1] = {0};
    if (read_query(request, query, sizeof(query)) != ESP_OK)
        return send_error_json(request, "400 Bad Request", "invalid_query",
                               "score query is too long or invalid");

    size_t requested_page = 1;
    size_t page_size = DEVICE_API_SCORE_PAGE_SIZE;
    if (query_size_value(query, "page", 1, 1000000, &requested_page) != ESP_OK ||
        query_size_value(query, "page_size", DEVICE_API_SCORE_PAGE_SIZE,
                         DEVICE_API_SCORE_PAGE_SIZE, &page_size) != ESP_OK)
        return send_error_json(request, "400 Bad Request", "invalid_page",
                               "page must be positive and page_size at most 20");

    char search[DEVICE_API_MAX_SEARCH_BYTES + 1] = {0};
    esp_err_t search_err = query_text_value(query, "search", search,
                                             sizeof(search));
    if (search_err != ESP_OK && search_err != ESP_ERR_NOT_FOUND)
        return send_error_json(request, "400 Bad Request", "invalid_search",
                               "search must be at most 64 bytes of UTF-8 text");

    score_info_t *entries = calloc(SCORE_LIST_MAX, sizeof(*entries));
    if (!entries)
        return send_error_json(request, "500 Internal Server Error",
                               "no_memory", "unable to allocate score list");

    int scanned = score_storage_scan_sd(entries, SCORE_LIST_MAX);
    size_t total = 0;
    if (scanned > 0) {
        for (int index = 0; index < scanned; ++index) {
            if (contains_search_text(entries[index].title, search) ||
                contains_search_text(entries[index].filename, search))
                ++total;
        }
    }

    size_t total_pages = total == 0 ? 0 : (total + page_size - 1) / page_size;
    size_t actual_page = total_pages > 0 && requested_page > total_pages
                             ? total_pages
                             : requested_page;
    size_t first_match = (actual_page - 1) * page_size;
    size_t matched = 0;
    size_t emitted = 0;

    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddBoolToObject(root, "ok", scanned >= 0);
        cJSON_AddBoolToObject(root, "sd_present", scanned >= 0);
        cJSON_AddStringToObject(root, "directory", "/sdcard/scores");
        cJSON_AddStringToObject(root, "search", search);
        cJSON_AddNumberToObject(root, "page", actual_page);
        cJSON_AddNumberToObject(root, "page_size", page_size);
        cJSON_AddNumberToObject(root, "total", total);
        cJSON_AddNumberToObject(root, "total_pages", total_pages);
        cJSON *scores = cJSON_AddArrayToObject(root, "scores");
        if (scores != NULL && scanned > 0) {
            for (int index = 0; index < scanned && emitted < page_size;
                 ++index) {
                score_info_t *score = &entries[index];
                if (!contains_search_text(score->title, search) &&
                    !contains_search_text(score->filename, search))
                    continue;
                if (matched++ < first_match) continue;

                cJSON *item = cJSON_CreateObject();
                if (!item) break;
                cJSON_AddStringToObject(item, "filename", score->filename);
                cJSON_AddStringToObject(item, "title", score->title);
                cJSON_AddStringToObject(item, "key", score->key);
                cJSON_AddStringToObject(item, "time_signature",
                                        score->time_signature);
                cJSON_AddNumberToObject(item, "bpm", score->bpm);
                cJSON_AddNumberToObject(item, "note_count",
                                        score->note_count);
                cJSON_AddNumberToObject(item, "measure_count",
                                        score->measure_count);
                cJSON_AddNumberToObject(item, "file_size",
                                        (double)score->file_size);
                cJSON_AddItemToArray(scores, item);
                ++emitted;
            }
        }
        if (scanned < 0)
            cJSON_AddStringToObject(root, "message",
                                    "SD score directory is unavailable");
    }
    free(entries);
    return send_json(request, "200 OK", root);
}

static esp_err_t sd_score_select_handler(httpd_req_t *request)
{
    cJSON *root = NULL;
    esp_err_t err = receive_json(request, &root);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request", "invalid_json",
                               "select request must be a JSON object");
    }
    cJSON *filename = cJSON_GetObjectItemCaseSensitive(root, "filename");
    if (!cJSON_IsString(filename) ||
        !score_storage_is_valid_sd_filename(filename->valuestring)) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request", "invalid_filename",
                               "a safe SD score JSON filename is required");
    }
    char safe_filename[64];
    strlcpy(safe_filename, filename->valuestring, sizeof(safe_filename));
    bool truncated = strlen(filename->valuestring) >= sizeof(safe_filename);
    cJSON_Delete(root);
    if (truncated)
        return send_error_json(request, "400 Bad Request", "invalid_filename",
                               "score filename is too long");
    err = screen_adapter_prepare_sd_score(safe_filename);
    if (err != ESP_OK)
        return send_error_json(
            request,
            err == ESP_ERR_INVALID_STATE ? "409 Conflict"
                                         : "400 Bad Request",
            err == ESP_ERR_INVALID_STATE
                ? "screen_preparation_conflict" : "score_load_failed",
            err == ESP_ERR_INVALID_STATE
                ? "screen is not ready or another mode is active"
                : "SD score is missing, too large, or invalid");

    score_data_status_t score = {0};
    score_data_get_status(&score);
    cJSON *response = cJSON_CreateObject();
    if (response != NULL) {
        cJSON_AddBoolToObject(response, "ok", true);
        cJSON_AddStringToObject(response, "filename", safe_filename);
        cJSON_AddStringToObject(response, "title", score.title);
        cJSON_AddNumberToObject(response, "bpm", score.bpm);
        cJSON_AddNumberToObject(response, "note_count", score.note_count);
        cJSON_AddStringToObject(response, "message", "SD score selected");
        add_preparation_status(response);
    }
    return send_json(request, "200 OK", response);
}

static esp_err_t sd_score_rename_handler(httpd_req_t *request)
{
    input_source_status_t input = {0};
    input_source_manager_get_status(&input);
    if (input.input_locked)
        return send_error_json(request, "409 Conflict", "practice_running",
                               "score title cannot change during practice");

    cJSON *root = NULL;
    esp_err_t err = receive_json(request, &root);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request", "invalid_json",
                               "rename request must be a JSON object");
    }

    cJSON *filename =
        cJSON_GetObjectItemCaseSensitive(root, "filename");
    cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "title");
    if (!cJSON_IsString(filename) || !cJSON_IsString(title) ||
        !score_storage_is_valid_sd_filename(filename->valuestring) ||
        !title->valuestring[0]) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request",
                               "invalid_score_rename",
                               "a safe filename and non-empty title are required");
    }

    char safe_filename[64];
    char safe_title[64];
    strlcpy(safe_filename, filename->valuestring, sizeof(safe_filename));
    strlcpy(safe_title, title->valuestring, sizeof(safe_title));
    bool truncated =
        strlen(filename->valuestring) >= sizeof(safe_filename) ||
        strlen(title->valuestring) >= sizeof(safe_title);
    cJSON_Delete(root);
    if (truncated)
        return send_error_json(request, "400 Bad Request",
                               "score_rename_too_long",
                               "filename or title is too long");

    err = score_storage_rename_sd_title(safe_filename, safe_title);
    if (err != ESP_OK) {
        const char *status = err == ESP_ERR_NOT_FOUND
                                 ? "404 Not Found" : "400 Bad Request";
        const char *code = err == ESP_ERR_NOT_FOUND
                               ? "score_not_found" : "score_rename_failed";
        return send_error_json(request, status, code,
                               "SD score title could not be changed");
    }

    cJSON *response = cJSON_CreateObject();
    if (response) {
        cJSON_AddBoolToObject(response, "ok", true);
        cJSON_AddStringToObject(response, "filename", safe_filename);
        cJSON_AddStringToObject(response, "title", safe_title);
        cJSON_AddStringToObject(response, "message",
                                "SD score title renamed");
    }
    return send_json(request, "200 OK", response);
}

static esp_err_t preparation_status_handler(httpd_req_t *request)
{
    cJSON *response = cJSON_CreateObject();
    if (response) {
        cJSON_AddBoolToObject(response, "ok", true);
        add_preparation_status(response);
    }
    return send_json(request, "200 OK", response);
}

static esp_err_t preparation_options_handler(httpd_req_t *request)
{
    cJSON *root = NULL;
    esp_err_t err = receive_json(request, &root);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request",
                               "invalid_preparation_options",
                               "options body must be a JSON object");
    }

    screen_preparation_status_t current = {0};
    screen_adapter_get_preparation_status(&current);
    if (!current.valid) {
        cJSON_Delete(root);
        return send_error_json(request, "409 Conflict",
                               "score_not_prepared",
                               "select a score before changing options");
    }

    screen_preparation_notation_t notation = current.notation;
    screen_preparation_mode_t mode = current.mode;
    screen_preparation_input_t input = current.input;
    cJSON *notation_json =
        cJSON_GetObjectItemCaseSensitive(root, "notation_type");
    cJSON *mode_json = cJSON_GetObjectItemCaseSensitive(root, "mode");
    cJSON *input_json =
        cJSON_GetObjectItemCaseSensitive(root, "input_source");

    bool valid = true;
    if (notation_json) {
        valid = cJSON_IsString(notation_json) &&
                (strcmp(notation_json->valuestring, "numbered") == 0 ||
                 strcmp(notation_json->valuestring, "staff") == 0);
        if (valid)
            notation = strcmp(notation_json->valuestring, "numbered") == 0
                           ? SCREEN_PREPARATION_NUMBERED
                           : SCREEN_PREPARATION_STAFF;
    }
    if (valid && mode_json) {
        valid = cJSON_IsString(mode_json) &&
                (strcmp(mode_json->valuestring, "read_only") == 0 ||
                 strcmp(mode_json->valuestring, "follow") == 0);
        if (valid)
            mode = strcmp(mode_json->valuestring, "read_only") == 0
                       ? SCREEN_PREPARATION_READ_ONLY
                       : SCREEN_PREPARATION_FOLLOW;
    }
    if (valid && input_json) {
        valid = cJSON_IsString(input_json) &&
                (strcmp(input_json->valuestring, "usb_midi") == 0 ||
                 strcmp(input_json->valuestring, "audio_s3") == 0);
        if (valid)
            input = strcmp(input_json->valuestring, "usb_midi") == 0
                        ? SCREEN_PREPARATION_USB_MIDI
                        : SCREEN_PREPARATION_AUDIO_S3;
    }
    cJSON_Delete(root);
    if (!valid)
        return send_error_json(
            request, "400 Bad Request", "invalid_preparation_options",
            "use numbered/staff, read_only/follow, and usb_midi/audio_s3");

    err = screen_adapter_update_preparation(notation, mode, input);
    if (err != ESP_OK)
        return send_error_json(request, "409 Conflict",
                               "preparation_update_failed",
                               "preparation options cannot change now");
    cJSON *response = cJSON_CreateObject();
    if (response) {
        cJSON_AddBoolToObject(response, "ok", true);
        add_preparation_status(response);
    }
    return send_json(request, "200 OK", response);
}

static esp_err_t preparation_start_handler(httpd_req_t *request)
{
    (void)request;
    esp_err_t err = screen_adapter_start_prepared_score();
    if (err != ESP_OK)
        return send_error_json(
            request, "409 Conflict", "preparation_start_failed",
            "score display or follow mode could not be started");
    cJSON *response = cJSON_CreateObject();
    if (response) {
        cJSON_AddBoolToObject(response, "ok", true);
        add_preparation_status(response);
    }
    return send_json(request, "200 OK", response);
}

static esp_err_t practice_restart_handler(httpd_req_t *request)
{
    screen_preparation_status_t preparation = {0};
    screen_adapter_get_preparation_status(&preparation);
    if (!preparation.valid) {
        return send_error_json(request, "409 Conflict",
                               "score_not_prepared",
                               "select a score before restarting practice");
    }

    esp_err_t err = screen_adapter_restart_prepared_score();
    if (err != ESP_OK) {
        scoring_service_status_t scoring = {0};
        scoring_service_get_status(&scoring);
        const bool scoring_now =
            scoring.state == SCORING_SERVICE_SCORING;
        return send_error_json(
            request, "409 Conflict",
            scoring_now ? "scoring_in_progress" : "practice_restart_failed",
            scoring_now ? "wait for the current score before restarting"
                        : "the prepared score could not be restarted");
    }

    cJSON *response = cJSON_CreateObject();
    if (response != NULL) {
        cJSON_AddBoolToObject(response, "ok", true);
        cJSON_AddStringToObject(response, "state", "recording");
        cJSON_AddStringToObject(response, "message",
                                "practice restarted with the same score");
        add_preparation_status(response);
    }
    return send_json(request, "200 OK", response);
}

static esp_err_t sd_score_file_handler(httpd_req_t *request)
{
    char query[DEVICE_API_MAX_QUERY_BYTES + 1] = {0};
    if (read_query(request, query, sizeof(query)) != ESP_OK)
        return send_error_json(request, "400 Bad Request", "invalid_query",
                               "score file query is invalid");

    char filename[64] = {0};
    if (query_text_value(query, "name", filename, sizeof(filename)) != ESP_OK ||
        !score_storage_is_valid_sd_filename(filename))
        return send_error_json(request, "400 Bad Request", "invalid_filename",
                               "a safe SD score JSON filename is required");

    char *json = NULL;
    size_t length = 0;
    if (!score_storage_read_sd_json(filename, &json, &length))
        return send_error_json(request, "404 Not Found", "score_not_found",
                               "SD score was not found or is not readable");
    httpd_resp_set_status(request, "200 OK");
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(request, json, length);
    free(json);
    return err;
}

static const char *creator_state_name(screen_creator_state_t state)
{
    switch (state) {
    case SCREEN_CREATOR_RECORDING: return "recording";
    case SCREEN_CREATOR_PAUSED: return "paused";
    case SCREEN_CREATOR_SAVING: return "saving";
    case SCREEN_CREATOR_ERROR: return "error";
    case SCREEN_CREATOR_IDLE:
    default: return "idle";
    }
}

static void add_creator_status(cJSON *root)
{
    screen_creator_status_t status = {0};
    screen_adapter_creator_get_status(&status);
    cJSON_AddBoolToObject(root, "available", status.available);
    cJSON_AddBoolToObject(root, "active", status.active);
    cJSON_AddStringToObject(root, "state", creator_state_name(status.state));
    cJSON_AddBoolToObject(root, "waiting_first_note",
                          status.waiting_first_note);
    cJSON_AddBoolToObject(root, "usb_midi_connected",
                          status.usb_midi_connected);
    cJSON_AddNumberToObject(root, "bpm", status.config.bpm);
    cJSON_AddNumberToObject(root, "time_sig_num",
                            status.config.time_sig_num);
    cJSON_AddNumberToObject(root, "time_sig_den",
                            status.config.time_sig_den);
    cJSON_AddStringToObject(
        root, "staff_mode",
        status.config.staff_mode == SCREEN_CREATOR_STAFF_GRAND
            ? "grand" : "single");
    cJSON_AddNumberToObject(root, "note_count", status.note_count);
    cJSON_AddNumberToObject(root, "measure_count", status.measure_count);
    cJSON_AddStringToObject(root, "last_error",
                            esp_err_to_name(status.last_error));
    cJSON_AddStringToObject(root, "saved_title", status.saved_title);
    cJSON_AddStringToObject(root, "saved_filename", status.saved_filename);
    cJSON_AddStringToObject(root, "message", status.message);
}

static esp_err_t creator_status_handler(httpd_req_t *request)
{
    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddBoolToObject(root, "ok", true);
        add_creator_status(root);
    }
    return send_json(request, "200 OK", root);
}

static esp_err_t creator_start_handler(httpd_req_t *request)
{
    cJSON *root = NULL;
    esp_err_t err = receive_json(request, &root);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request",
                               "invalid_creator_config",
                               "Creator config must be a JSON object");
    }
    cJSON *bpm = cJSON_GetObjectItemCaseSensitive(root, "bpm");
    cJSON *time_num = cJSON_GetObjectItemCaseSensitive(root, "time_sig_num");
    cJSON *time_den = cJSON_GetObjectItemCaseSensitive(root, "time_sig_den");
    cJSON *staff = cJSON_GetObjectItemCaseSensitive(root, "staff_mode");
    if (!cJSON_IsNumber(bpm) || !cJSON_IsNumber(time_num) ||
        !cJSON_IsNumber(time_den) || !cJSON_IsString(staff)) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request",
                               "invalid_creator_config",
                               "bpm, time signature, and staff mode are required");
    }
    screen_creator_config_t config = {
        .bpm = bpm->valueint,
        .time_sig_num = time_num->valueint,
        .time_sig_den = time_den->valueint,
        .staff_mode = strcmp(staff->valuestring, "grand") == 0
                          ? SCREEN_CREATOR_STAFF_GRAND
                          : strcmp(staff->valuestring, "single") == 0
                                ? SCREEN_CREATOR_STAFF_SINGLE
                                : 0,
    };
    cJSON_Delete(root);
    if (config.staff_mode == 0)
        return send_error_json(request, "400 Bad Request",
                               "invalid_creator_config",
                               "staff_mode must be single or grand");

    err = screen_adapter_creator_start(&config);
    if (err != ESP_OK)
        return send_error_json(
            request,
            err == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "409 Conflict",
            err == ESP_ERR_INVALID_ARG ? "invalid_creator_config"
                                       : "creator_start_conflict",
            err == ESP_ERR_INVALID_ARG
                ? "use a supported BPM, time signature, and staff mode"
                : "Creator Mode or practice is already active");
    cJSON *response = cJSON_CreateObject();
    if (response != NULL) {
        cJSON_AddBoolToObject(response, "ok", true);
        add_creator_status(response);
    }
    return send_json(request, "200 OK", response);
}

static esp_err_t creator_action_handler(httpd_req_t *request)
{
    cJSON *root = NULL;
    esp_err_t err = receive_json(request, &root);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request",
                               "invalid_creator_action",
                               "Creator action must be a JSON object");
    }
    cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!cJSON_IsString(action)) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request",
                               "invalid_creator_action",
                               "action is required");
    }
    char action_copy[16];
    strlcpy(action_copy, action->valuestring, sizeof(action_copy));
    bool truncated = strlen(action->valuestring) >= sizeof(action_copy);
    cJSON_Delete(root);
    if (truncated)
        return send_error_json(request, "400 Bad Request",
                               "invalid_creator_action",
                               "Creator action is too long");

    if (strcmp(action_copy, "pause") == 0)
        err = screen_adapter_creator_pause();
    else if (strcmp(action_copy, "resume") == 0)
        err = screen_adapter_creator_resume();
    else if (strcmp(action_copy, "finish") == 0)
        err = screen_adapter_creator_finish();
    else if (strcmp(action_copy, "cancel") == 0)
        err = screen_adapter_creator_cancel();
    else
        return send_error_json(request, "400 Bad Request",
                               "invalid_creator_action",
                               "action must be pause, resume, finish, or cancel");

    if (err != ESP_OK)
        return send_error_json(
            request, "409 Conflict", "creator_action_conflict",
            strcmp(action_copy, "finish") == 0
                ? "pause first and record at least one note before saving"
                : "Creator action is not allowed in the current state");
    cJSON *response = cJSON_CreateObject();
    if (response != NULL) {
        cJSON_AddBoolToObject(response, "ok", true);
        add_creator_status(response);
    }
    return send_json(request, "200 OK", response);
}

static esp_err_t practice_start_handler(httpd_req_t *request)
{
    cJSON *root = NULL;
    esp_err_t err = receive_optional_json(request, &root);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request",
                               "invalid_start_request",
                               "start body must be an optional JSON object");
    }
    cJSON *source_json = cJSON_GetObjectItemCaseSensitive(root,
                                                          "input_source");
    cJSON *profile_json = cJSON_GetObjectItemCaseSensitive(root, "profile");
    if ((source_json != NULL && !cJSON_IsString(source_json)) ||
        (profile_json != NULL && !cJSON_IsString(profile_json))) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request",
                               "invalid_start_profile",
                               "input_source and profile must be strings");
    }
    const char *profile = cJSON_IsString(profile_json)
                              ? profile_json->valuestring
                              : "beginner_mono_v2";
    char profile_copy[SCORING_PROFILE_NAME_MAX_LENGTH];
    strlcpy(profile_copy, profile, sizeof(profile_copy));
    bool profile_truncated = strlen(profile) >= sizeof(profile_copy);

    input_source_status_t input;
    input_source_manager_get_status(&input);
    bool source_mismatch = false;
    bool source_invalid = false;
    if (cJSON_IsString(source_json)) {
        input_source_t requested_source = INPUT_SOURCE_NONE;
        source_invalid =
            !input_source_from_name(source_json->valuestring,
                                    &requested_source);
        source_mismatch = !source_invalid &&
                          requested_source != input.selected_input;
    }
    cJSON_Delete(root);
    if (profile_truncated) {
        return send_error_json(request, "400 Bad Request",
                               "invalid_start_profile",
                               "profile is too long");
    }
    if (source_invalid) {
        return send_error_json(request, "400 Bad Request",
                               "invalid_input_source",
                               "input_source must be usb_midi or audio_s3");
    }
    if (input.input_locked) {
        return send_error_json(request, "409 Conflict", "practice_running",
                               "input source is locked during practice");
    }
    if (source_mismatch) {
        return send_error_json(
            request, "409 Conflict", "input_source_mismatch",
            "start input_source does not match the manually selected input");
    }
    screen_creator_status_t creator = {0};
    screen_adapter_creator_get_status(&creator);
    if (creator.active) {
        return send_error_json(request, "409 Conflict",
                               "creator_mode_running",
                               "finish or cancel Creator Mode before practice");
    }

    screen_preparation_status_t preparation = {0};
    screen_adapter_get_preparation_status(&preparation);
    if (preparation.valid) {
        err = screen_adapter_start_prepared_score();
        if (err != ESP_OK)
            return send_error_json(request, "409 Conflict",
                                   "preparation_start_failed",
                                   "prepared score could not be displayed");
        cJSON *response = cJSON_CreateObject();
        if (response) {
            cJSON_AddBoolToObject(response, "ok", true);
            cJSON_AddStringToObject(response, "message",
                                    "prepared score started");
            add_preparation_status(response);
        }
        return send_json(request, "200 OK", response);
    }

    score_data_status_t score;
    score_data_get_status(&score);
    if (!score.loaded) {
        return send_error_json(request, "409 Conflict", "score_not_loaded",
                               "upload a valid score before starting");
    }
    if (input.selected_input == INPUT_SOURCE_USB_MIDI &&
        !input.usb_midi_connected) {
        return send_error_json(request, "409 Conflict",
                               "usb_midi_not_connected",
                               "未检测到 USB MIDI 电子琴");
    }
    if (input.selected_input == INPUT_SOURCE_AUDIO_S3) {
        if (!input.audio_s3_implemented) {
            return send_error_json(request, "409 Conflict",
                                   "audio_s3_not_implemented",
                                   "S3 麦克风音频输入暂未实现");
        }
        if (!input.audio_s3_connected) {
            return send_error_json(request, "409 Conflict",
                                   "audio_s3_not_connected",
                                   "未检测到 S3 音频节点");
        }
    }
    if (strcmp(profile_copy, "beginner_mono_v2") != 0 &&
        strcmp(profile_copy, "midi_strict") != 0) {
        return send_error_json(request, "409 Conflict",
                               "unsupported_scoring_profile",
                               "the selected scoring profile is unavailable");
    }

    const char *selected_source = input_source_name(input.selected_input);
    err = scoring_service_start(selected_source, profile_copy);
    if (err != ESP_OK) {
        scoring_service_status_t practice;
        scoring_service_get_status(&practice);
        const char *code = err == ESP_ERR_NOT_SUPPORTED
                               ? "unsupported_input_or_profile"
                               : practice.error[0] != '\0'
                                     ? practice.error
                                     : "practice_start_conflict";
        const char *message = practice.message[0] != '\0'
                                  ? practice.message
                                  : "practice cannot start in the current state";
        return send_error_json(request, "409 Conflict", code, message);
    }

    cJSON *response = cJSON_CreateObject();
    if (response != NULL) {
        cJSON_AddBoolToObject(response, "ok", true);
        cJSON_AddStringToObject(response, "state", "recording");
        cJSON_AddStringToObject(response, "message", "recording started");
        cJSON_AddStringToObject(response, "input_source", selected_source);
        cJSON_AddStringToObject(response, "profile", profile_copy);
    }
    return send_json(request, "200 OK", response);
}

typedef struct {
    httpd_req_t *request;
    char session_id[SCORING_SESSION_ID_MAX_LENGTH];
} score_result_send_context_t;

static esp_err_t send_score_result(const char *json,
                                   size_t length,
                                   void *context);

static esp_err_t send_current_score_result(httpd_req_t *request)
{
    scoring_service_status_t status;
    scoring_service_get_status(&status);
    score_result_send_context_t context = {.request = request};
    strlcpy(context.session_id, status.practice_session_id,
            sizeof(context.session_id));
    httpd_resp_set_status(request, "200 OK");
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return scoring_service_with_result(send_score_result, &context);
}

static esp_err_t practice_stop_handler(httpd_req_t *request)
{
    esp_err_t err = screen_adapter_complete_practice();
    if (err != ESP_OK) {
        scoring_service_status_t status;
        scoring_service_get_status(&status);
        return send_error_json(
            request, "409 Conflict",
            status.error[0] != '\0' ? status.error : "practice_not_recording",
            status.message[0] != '\0'
                ? status.message
                : "there is no active recording to stop");
    }
    err = scoring_service_wait_for_result(DEVICE_API_SCORING_TIMEOUT_MS);
    if (err == ESP_ERR_TIMEOUT) {
        return send_error_json(
            request, "504 Gateway Timeout", "scoring_timeout",
            "local scoring did not finish within 10 seconds; use /api/result to retrieve it later");
    }
    if (err != ESP_OK) {
        scoring_service_status_t status;
        scoring_service_get_status(&status);
        return send_error_json(
            request, "500 Internal Server Error",
            status.error[0] != '\0' ? status.error : "scoring_failed",
            status.message[0] != '\0'
                ? status.message
                : "local scoring failed");
    }

    return send_current_score_result(request);
}

static esp_err_t send_score_result(const char *json,
                                   size_t length,
                                   void *context)
{
    score_result_send_context_t *send = context;
    if (send == NULL || send->request == NULL || json == NULL ||
        length < 2 || json[0] != '{') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = httpd_resp_send_chunk(
        send->request, "{\"practice_session_id\":\"",
        HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(send->request, send->session_id,
                                    HTTPD_RESP_USE_STRLEN);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(send->request, "\",", 2);
    }
    if (err == ESP_OK) {
        size_t offset = 1;
        while (err == ESP_OK && offset < length) {
            size_t chunk_length = length - offset;
            if (chunk_length > DEVICE_API_SCORE_RESULT_CHUNK_BYTES) {
                chunk_length = DEVICE_API_SCORE_RESULT_CHUNK_BYTES;
            }
            err = httpd_resp_send_chunk(send->request, json + offset,
                                        chunk_length);
            offset += chunk_length;
            if (err == ESP_OK && offset < length) {
                /* Bound each TCP/SDIO burst and let the transport drain. */
                vTaskDelay(1);
            }
        }
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(send->request, NULL, 0);
    }
    return err;
}

static esp_err_t practice_result_handler(httpd_req_t *request)
{
    scoring_service_status_t status;
    scoring_service_get_status(&status);
    if (status.state == SCORING_SERVICE_SCORING) {
        cJSON *root = cJSON_CreateObject();
        if (root != NULL) {
            cJSON_AddBoolToObject(root, "ok", true);
            cJSON_AddBoolToObject(root, "ready", false);
            cJSON_AddStringToObject(root, "state", "scoring");
            cJSON_AddStringToObject(root, "message", "scoring in progress");
        }
        return send_json(request, "202 Accepted", root);
    }
    if (status.state == SCORING_SERVICE_ERROR) {
        return send_error_json(
            request, "409 Conflict",
            status.error[0] != '\0' ? status.error : "scoring_failed",
            status.message[0] != '\0' ? status.message : "scoring failed");
    }
    if (status.state != SCORING_SERVICE_READY) {
        return send_error_json(request, "404 Not Found",
                               "score_result_not_available",
                               status.state == SCORING_SERVICE_RECORDING
                                   ? "stop the recording before requesting a result"
                                   : "no completed scoring result is available");
    }

    return send_current_score_result(request);
}

static esp_err_t volume_handler(httpd_req_t *request)
{
    cJSON *root = NULL;
    esp_err_t err = receive_json(request, &root);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request",
                               "INVALID_JSON", "音量请求格式错误");
    }
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (!cJSON_IsNumber(value)) {
        value = cJSON_GetObjectItemCaseSensitive(root, "percent");
    }
    if (!cJSON_IsNumber(value) || value->valuedouble < 0 ||
        value->valuedouble > 80) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request",
                               "INVALID_VOLUME", "设备音量必须在 0 到 80 之间");
    }
    uint8_t percent = (uint8_t)value->valueint;
    cJSON_Delete(root);
    return send_command_result(request,
                               speaker_service_set_volume(percent),
                               "音量命令已加入队列");
}

static esp_err_t mute_handler(httpd_req_t *request)
{
    cJSON *root = NULL;
    esp_err_t err = receive_json(request, &root);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request",
                               "INVALID_JSON", "静音请求格式错误");
    }
    cJSON *muted = cJSON_GetObjectItemCaseSensitive(root, "muted");
    if (!cJSON_IsBool(muted)) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request",
                               "INVALID_MUTE", "muted 必须是布尔值");
    }
    bool value = cJSON_IsTrue(muted);
    cJSON_Delete(root);
    return send_command_result(request,
                               speaker_service_set_mute(value),
                               value ? "静音命令已加入队列"
                                     : "取消静音命令已加入队列");
}

static esp_err_t tone_handler(httpd_req_t *request)
{
    cJSON *root = NULL;
    esp_err_t err = receive_json(request, &root);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request",
                               "INVALID_JSON", "校准音请求格式错误");
    }
    cJSON *frequency = cJSON_GetObjectItemCaseSensitive(root, "frequency_hz");
    cJSON *duration = cJSON_GetObjectItemCaseSensitive(root, "duration_ms");
    cJSON *gain_item = cJSON_GetObjectItemCaseSensitive(root, "gain");
    float gain = cJSON_IsNumber(gain_item) ? (float)gain_item->valuedouble : 0.12f;
    if (!cJSON_IsNumber(frequency) || !cJSON_IsNumber(duration)) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request",
                               "INVALID_TONE", "缺少 frequency_hz 或 duration_ms");
    }
    float frequency_hz = (float)frequency->valuedouble;
    uint32_t duration_ms = (uint32_t)duration->valuedouble;
    cJSON_Delete(root);
    return send_command_result(
        request,
        speaker_service_play_tone(frequency_hz, duration_ms, gain),
        "校准音命令已加入队列");
}

static esp_err_t stop_handler(httpd_req_t *request)
{
    return send_command_result(request,
                               speaker_service_stop(),
                               "停止命令已加入队列");
}

static esp_err_t metronome_start_handler(httpd_req_t *request)
{
    cJSON *root = NULL;
    esp_err_t err = receive_json(request, &root);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request",
                               "INVALID_JSON", "节拍器请求格式错误");
    }
    cJSON *bpm = cJSON_GetObjectItemCaseSensitive(root, "bpm");
    cJSON *beats = cJSON_GetObjectItemCaseSensitive(root,
                                                    "beats_per_measure");
    cJSON *unit = cJSON_GetObjectItemCaseSensitive(root, "beat_unit");
    if (!cJSON_IsNumber(bpm) || !cJSON_IsNumber(beats) ||
        !cJSON_IsNumber(unit)) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request",
                               "INVALID_METRONOME",
                               "缺少 bpm、beats_per_measure 或 beat_unit");
    }
    uint16_t bpm_value = (uint16_t)bpm->valueint;
    uint8_t beats_value = (uint8_t)beats->valueint;
    uint8_t unit_value = (uint8_t)unit->valueint;
    cJSON_Delete(root);
    return send_command_result(
        request,
        speaker_service_metronome_start(bpm_value, beats_value, unit_value),
        "节拍器命令已加入队列");
}

static esp_err_t metronome_pause_handler(httpd_req_t *request)
{
    return send_command_result(request,
                               speaker_service_metronome_pause(),
                               "节拍器暂停命令已加入队列");
}

static esp_err_t metronome_stop_handler(httpd_req_t *request)
{
    return send_command_result(request,
                               speaker_service_metronome_stop(),
                               "节拍器停止命令已加入队列");
}

static esp_err_t files_handler(httpd_req_t *request)
{
    char query[DEVICE_API_MAX_QUERY_BYTES + 1] = {0};
    size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length > DEVICE_API_MAX_QUERY_BYTES ||
        (query_length > 0 &&
         httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK)) {
        return send_error_json(request, "400 Bad Request", "INVALID_QUERY",
                               "文件查询参数过长或格式错误");
    }

    size_t requested_page = 1;
    size_t page_size = DEVICE_API_AUDIO_PAGE_SIZE;
    if (query_size_value(query, "page", 1, 1000000, &requested_page) != ESP_OK ||
        query_size_value(query, "page_size", DEVICE_API_AUDIO_PAGE_SIZE,
                         DEVICE_API_AUDIO_PAGE_SIZE, &page_size) != ESP_OK) {
        return send_error_json(request, "400 Bad Request", "INVALID_PAGE",
                               "page 必须大于 0，page_size 必须在 1 到 10 之间");
    }

    char search[DEVICE_API_MAX_SEARCH_BYTES + 1] = {0};
    char encoded_search[DEVICE_API_MAX_QUERY_BYTES + 1] = {0};
    if (query[0] != '\0' &&
        httpd_query_key_value(query, "search", encoded_search,
                              sizeof(encoded_search)) == ESP_OK &&
        decode_query_value(encoded_search, search, sizeof(search)) != ESP_OK) {
        return send_error_json(request, "400 Bad Request", "INVALID_SEARCH",
                               "搜索词必须是最长 64 字节的 UTF-8 文本");
    }

    speaker_status_t status;
    memset(&status, 0, sizeof(status));
    speaker_service_get_status(&status);

    const size_t capacity = DEVICE_API_AUDIO_PAGE_SIZE;
    char (*names)[SPEAKER_FILE_NAME_MAX] =
        calloc(capacity, sizeof(*names));
    if (names == NULL) {
        return send_error_json(request, "500 Internal Server Error",
                               "NO_MEMORY", "无法分配文件列表内存");
    }
    size_t count = 0;
    size_t total = 0;
    size_t actual_page = 1;
    esp_err_t err = status.sd_present
                        ? speaker_service_list_files_page(
                              search, requested_page, page_size, names,
                              capacity, &count, &total, &actual_page)
                        : ESP_ERR_INVALID_STATE;

    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddBoolToObject(root, "ok",
                              err == ESP_OK || err == ESP_ERR_NOT_FOUND);
        cJSON_AddBoolToObject(root, "sd_present", status.sd_present);
        cJSON_AddStringToObject(root, "directory", "/sdcard/wav");
        cJSON_AddStringToObject(root, "search", search);
        cJSON_AddNumberToObject(root, "page", actual_page);
        cJSON_AddNumberToObject(root, "page_size", page_size);
        cJSON_AddNumberToObject(root, "total", total);
        cJSON_AddNumberToObject(
            root, "total_pages",
            total == 0 ? 0 : (total + page_size - 1) / page_size);
        cJSON *files = cJSON_AddArrayToObject(root, "files");
        if (files != NULL && err == ESP_OK) {
            for (size_t index = 0; index < count; ++index) {
                cJSON_AddItemToArray(files, cJSON_CreateString(names[index]));
            }
        }
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
        }
    }
    free(names);
    return send_json(request, "200 OK", root);
}

static esp_err_t file_play_handler(httpd_req_t *request)
{
    cJSON *root = NULL;
    esp_err_t err = receive_json(request, &root);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request",
                               "INVALID_JSON", "WAV 播放请求格式错误");
    }
    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(name) || name->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_error_json(request, "400 Bad Request",
                               "INVALID_FILE", "缺少 WAV 文件名");
    }
    char safe_name[SPEAKER_FILE_NAME_MAX];
    strlcpy(safe_name, name->valuestring, sizeof(safe_name));
    bool truncated = strlen(name->valuestring) >= sizeof(safe_name);
    cJSON_Delete(root);
    if (truncated) {
        return send_error_json(request, "400 Bad Request",
                               "INVALID_FILE", "WAV 文件名过长");
    }
    return send_command_result(request,
                               speaker_service_play_file(safe_name),
                               "WAV 播放命令已加入队列");
}

static esp_err_t file_pause_handler(httpd_req_t *request)
{
    return send_command_result(request,
                               speaker_service_pause_file(),
                               "WAV 暂停或继续命令已加入队列");
}

static esp_err_t file_stop_handler(httpd_req_t *request)
{
    return send_command_result(request,
                               speaker_service_stop_file(),
                               "WAV 停止命令已加入队列");
}

static bool safe_wav_upload_name(const char *name)
{
    if (name == NULL || name[0] == '\0' ||
        strlen(name) >= SPEAKER_FILE_NAME_MAX ||
        strstr(name, "..") != NULL || strchr(name, '/') != NULL ||
        strchr(name, '\\') != NULL) {
        return false;
    }
    size_t length = strlen(name);
    return length > 4 && strcasecmp(name + length - 4, ".wav") == 0;
}

static bool audio_import_allowed(void)
{
    scoring_service_status_t practice = {0};
    scoring_service_get_status(&practice);
    if (practice.state == SCORING_SERVICE_RECORDING ||
        practice.state == SCORING_SERVICE_PAUSED ||
        practice.state == SCORING_SERVICE_SCORING) {
        return false;
    }
    speaker_status_t audio = {0};
    speaker_service_get_status(&audio);
    return audio.sd_present &&
           (audio.state == SPEAKER_STATE_STOPPED ||
            audio.state == SPEAKER_STATE_ERROR);
}

static uint16_t read_le16(const uint8_t *value)
{
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static uint32_t read_le32(const uint8_t *value)
{
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
           ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

static bool validate_uploaded_wav(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    uint8_t riff[12];
    bool valid = fread(riff, 1, sizeof(riff), file) == sizeof(riff) &&
                 memcmp(riff, "RIFF", 4) == 0 &&
                 memcmp(riff + 8, "WAVE", 4) == 0;
    bool format_ok = false;
    bool data_found = false;
    while (valid && !(format_ok && data_found)) {
        uint8_t chunk[8];
        if (fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) break;
        uint32_t size = read_le32(chunk + 4);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t format[16];
            if (size < sizeof(format) ||
                fread(format, 1, sizeof(format), file) != sizeof(format)) {
                valid = false;
                break;
            }
            format_ok = read_le16(format) == 1 &&
                        read_le16(format + 2) == 1 &&
                        (read_le32(format + 4) == 16000 ||
                         read_le32(format + 4) == 24000) &&
                        read_le16(format + 14) == 16;
            if (size > sizeof(format) &&
                fseek(file, (long)(size - sizeof(format)), SEEK_CUR) != 0) {
                valid = false;
            }
        } else if (memcmp(chunk, "data", 4) == 0) {
            data_found = size > 0;
            if (!format_ok && fseek(file, (long)size, SEEK_CUR) != 0) {
                valid = false;
            }
        } else if (fseek(file, (long)size, SEEK_CUR) != 0) {
            valid = false;
        }
        if (valid && (size & 1U) != 0 && fseek(file, 1, SEEK_CUR) != 0) {
            valid = false;
        }
    }
    fclose(file);
    return valid && format_ok && data_found;
}

static esp_err_t audio_upload_handler(httpd_req_t *request)
{
    if (!audio_import_allowed()) {
        return send_error_json(request, "409 Conflict", "device_busy",
                               "练习、评分或音乐播放期间不能上传音乐");
    }
    if (request->content_len == 0 ||
        request->content_len > DEVICE_API_AUDIO_UPLOAD_CHUNK_BYTES) {
        return send_error_json(request, "413 Payload Too Large",
                               "invalid_audio_chunk",
                               "单个上传分块不能超过 256KB");
    }

    char query[DEVICE_API_MAX_QUERY_BYTES + 1] = {0};
    size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length == 0 || query_length > DEVICE_API_MAX_QUERY_BYTES ||
        httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) {
        return send_error_json(request, "400 Bad Request", "invalid_query",
                               "缺少音乐上传参数");
    }
    char encoded_name[DEVICE_API_MAX_QUERY_BYTES + 1] = {0};
    char name[SPEAKER_FILE_NAME_MAX] = {0};
    if (httpd_query_key_value(query, "name", encoded_name,
                              sizeof(encoded_name)) != ESP_OK ||
        decode_query_value(encoded_name, name, sizeof(name)) != ESP_OK ||
        !safe_wav_upload_name(name)) {
        return send_error_json(request, "400 Bad Request", "invalid_file",
                               "文件名必须是安全的 WAV 文件名");
    }
    char offset_text[24] = {0};
    if (httpd_query_key_value(query, "offset", offset_text,
                              sizeof(offset_text)) != ESP_OK) {
        return send_error_json(request, "400 Bad Request", "invalid_offset",
                               "缺少上传偏移量");
    }
    char *end = NULL;
    unsigned long long parsed_offset = strtoull(offset_text, &end, 10);
    if (offset_text[0] == '\0' || end == NULL || *end != '\0' ||
        parsed_offset > DEVICE_API_AUDIO_UPLOAD_MAX_BYTES) {
        return send_error_json(request, "400 Bad Request", "invalid_offset",
                               "上传偏移量无效");
    }
    size_t offset = (size_t)parsed_offset;
    if (offset + request->content_len > DEVICE_API_AUDIO_UPLOAD_MAX_BYTES) {
        return send_error_json(request, "413 Payload Too Large",
                               "audio_too_large", "WAV 文件不能超过 64MB");
    }
    char final_text[4] = {0};
    bool final_chunk = httpd_query_key_value(query, "final", final_text,
                                              sizeof(final_text)) == ESP_OK &&
                       strcmp(final_text, "1") == 0;

    if (mkdir(BOARD_SDCARD_WAV_DIRECTORY, 0775) != 0 && errno != EEXIST) {
        return send_error_json(request, "500 Internal Server Error",
                               "sd_write_failed", "无法创建 SD 音乐目录");
    }
    char final_path[sizeof(BOARD_SDCARD_WAV_DIRECTORY) +
                    SPEAKER_FILE_NAME_MAX + 2];
    char part_path[sizeof(BOARD_SDCARD_WAV_DIRECTORY) +
                   SPEAKER_FILE_NAME_MAX + 8];
    snprintf(final_path, sizeof(final_path), "%s/%s",
             BOARD_SDCARD_WAV_DIRECTORY, name);
    snprintf(part_path, sizeof(part_path), "%s/.%s.part",
             BOARD_SDCARD_WAV_DIRECTORY, name);

    struct stat info = {0};
    if (offset > 0 &&
        (stat(part_path, &info) != 0 || (size_t)info.st_size != offset)) {
        return send_error_json(request, "409 Conflict", "offset_mismatch",
                               "上传进度不匹配，请重新上传");
    }
    FILE *file = fopen(part_path, offset == 0 ? "wb" : "r+b");
    if (file == NULL || (offset > 0 && fseek(file, (long)offset, SEEK_SET) != 0)) {
        if (file != NULL) fclose(file);
        return send_error_json(request, "500 Internal Server Error",
                               "sd_write_failed", "无法打开 SD 临时文件");
    }

    uint8_t buffer[4096];
    size_t received = 0;
    bool write_ok = true;
    while (received < request->content_len) {
        size_t wanted = request->content_len - received;
        if (wanted > sizeof(buffer)) wanted = sizeof(buffer);
        int count = httpd_req_recv(request, (char *)buffer, wanted);
        if (count == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (count <= 0 || fwrite(buffer, 1, (size_t)count, file) != (size_t)count) {
            write_ok = false;
            break;
        }
        received += (size_t)count;
    }
    if (!write_ok || fflush(file) != 0) {
        (void)ftruncate(fileno(file), (off_t)offset);
        fclose(file);
        return send_error_json(request, "500 Internal Server Error",
                               "sd_write_failed", "音乐分块写入失败");
    }
    fclose(file);

    size_t next_offset = offset + received;
    if (final_chunk) {
        if (!validate_uploaded_wav(part_path)) {
            remove(part_path);
            return send_error_json(request, "400 Bad Request", "invalid_wav",
                                   "转码结果不是受支持的 WAV 格式");
        }
        remove(final_path);
        if (rename(part_path, final_path) != 0) {
            return send_error_json(request, "500 Internal Server Error",
                                   "sd_write_failed", "无法完成音乐文件保存");
        }
    }

    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "name", name);
        cJSON_AddNumberToObject(root, "received", next_offset);
        cJSON_AddBoolToObject(root, "complete", final_chunk);
    }
    return send_json(request, "200 OK", root);
}

static bool practice_media_busy(void)
{
    scoring_service_status_t practice = {0};
    scoring_service_get_status(&practice);
    return practice.state == SCORING_SERVICE_RECORDING ||
           practice.state == SCORING_SERVICE_PAUSED ||
           practice.state == SCORING_SERVICE_SCORING;
}

static esp_err_t query_practice_session(httpd_req_t *request,
                                        char *session_id,
                                        size_t capacity,
                                        char *query,
                                        size_t query_capacity)
{
    size_t length = httpd_req_get_url_query_len(request);
    if (length == 0 || length + 1 > query_capacity ||
        httpd_req_get_url_query_str(request, query, query_capacity) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    char encoded[DEVICE_API_MAX_QUERY_BYTES + 1] = {0};
    if (httpd_query_key_value(query, "session_id", encoded,
                              sizeof(encoded)) != ESP_OK ||
        decode_query_value(encoded, session_id, capacity) != ESP_OK ||
        session_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t practice_photos_handler(httpd_req_t *request)
{
    if (practice_media_busy()) {
        return send_error_json(request, "409 Conflict", "device_busy",
                               "练习或评分期间暂停读取照片");
    }
    char query[DEVICE_API_MAX_QUERY_BYTES + 1] = {0};
    char session_id[64] = {0};
    if (query_practice_session(request, session_id, sizeof(session_id),
                               query, sizeof(query)) != ESP_OK) {
        return send_error_json(request, "400 Bad Request", "invalid_session",
                               "缺少有效的练习会话编号");
    }
    size_t page = 1;
    size_t page_size = DEVICE_API_PHOTO_PAGE_SIZE;
    if (query_size_value(query, "page", 1, 1000000, &page) != ESP_OK ||
        query_size_value(query, "page_size", DEVICE_API_PHOTO_PAGE_SIZE,
                         DEVICE_API_PHOTO_PAGE_SIZE, &page_size) != ESP_OK) {
        return send_error_json(request, "400 Bad Request", "invalid_page",
                               "照片分页参数无效");
    }
    uint32_t count = 0;
    esp_err_t err = s3_camera_node_list_photos(session_id, &count);
    if (err != ESP_OK) {
        return send_error_json(request,
                               err == ESP_ERR_TIMEOUT
                                   ? "504 Gateway Timeout" : "404 Not Found",
                               "photos_unavailable",
                               "摄像头板离线、SD 卡不可用或没有本次照片");
    }
    size_t total_pages = count == 0 ? 0 :
                         ((size_t)count + page_size - 1) / page_size;
    if (total_pages > 0 && page > total_pages) page = total_pages;
    size_t start = (page - 1) * page_size + 1;
    size_t end_index = start + page_size;
    if (end_index > (size_t)count + 1) end_index = (size_t)count + 1;
    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "session_id", session_id);
        cJSON_AddNumberToObject(root, "total", count);
        cJSON_AddNumberToObject(root, "page", page);
        cJSON_AddNumberToObject(root, "page_size", page_size);
        cJSON_AddNumberToObject(root, "total_pages", total_pages);
        cJSON *photos = cJSON_AddArrayToObject(root, "photos");
        for (size_t index = start; photos != NULL && index < end_index; ++index) {
            cJSON *item = cJSON_CreateObject();
            if (item == NULL) break;
            cJSON_AddNumberToObject(item, "index", index);
            char name[16];
            snprintf(name, sizeof(name), "I%06u.JPG", (unsigned)index);
            cJSON_AddStringToObject(item, "name", name);
            cJSON_AddItemToArray(photos, item);
        }
    }
    return send_json(request, "200 OK", root);
}

typedef struct {
    httpd_req_t *request;
    bool started;
} photo_http_context_t;

static esp_err_t send_photo_http_chunk(const uint8_t *data,
                                       size_t length,
                                       size_t total_length,
                                       bool first_chunk,
                                       void *context)
{
    photo_http_context_t *photo = (photo_http_context_t *)context;
    if (photo == NULL || photo->request == NULL) return ESP_ERR_INVALID_ARG;
    if (first_chunk) {
        (void)total_length;
        httpd_resp_set_type(photo->request, "image/jpeg");
        httpd_resp_set_hdr(photo->request, "Cache-Control", "no-store");
        photo->started = true;
    }
    return httpd_resp_send_chunk(photo->request, (const char *)data, length);
}

static esp_err_t practice_photo_handler(httpd_req_t *request)
{
    if (practice_media_busy()) {
        return send_error_json(request, "409 Conflict", "device_busy",
                               "练习或评分期间暂停读取照片");
    }
    char query[DEVICE_API_MAX_QUERY_BYTES + 1] = {0};
    char session_id[64] = {0};
    if (query_practice_session(request, session_id, sizeof(session_id),
                               query, sizeof(query)) != ESP_OK) {
        return send_error_json(request, "400 Bad Request", "invalid_session",
                               "缺少有效的练习会话编号");
    }
    char index_text[16] = {0};
    if (httpd_query_key_value(query, "index", index_text,
                              sizeof(index_text)) != ESP_OK) {
        return send_error_json(request, "400 Bad Request", "invalid_photo",
                               "缺少照片编号");
    }
    char *end = NULL;
    unsigned long index = strtoul(index_text, &end, 10);
    if (index == 0 || index > 999999 || end == NULL || *end != '\0') {
        return send_error_json(request, "400 Bad Request", "invalid_photo",
                               "照片编号无效");
    }
    photo_http_context_t context = {.request = request, .started = false};
    esp_err_t err = s3_camera_node_stream_photo(
        session_id, (uint32_t)index, send_photo_http_chunk, &context);
    if (context.started) {
        (void)httpd_resp_send_chunk(request, NULL, 0);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return send_error_json(request,
                               err == ESP_ERR_TIMEOUT
                                   ? "504 Gateway Timeout" : "404 Not Found",
                               "photo_unavailable",
                               "照片读取失败，请检查摄像头板和 SD 卡");
    }
    return ESP_OK;
}

esp_err_t device_api_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }
    network_status_t network = network_provisioning_get_status();
    if (network.state != NETWORK_STATE_WIFI_CONNECTED ||
        network.ip[0] == '\0' || strcmp(network.ip, "0.0.0.0") == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 41;
    config.max_open_sockets = DEVICE_API_MAX_OPEN_SOCKETS;
    config.stack_size = 16384;
    /* Request handlers can commit NVS and access partition-backed storage.
     * IDF 5.5.3 requires the active task stack to remain accessible while
     * flash disables cache. Request bodies and other large buffers still use
     * their existing PSRAM allocations. */
    config.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    config.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG,
             "socket budget ready: http_clients=%u http_internal=3 lwip_max=%d",
             (unsigned)config.max_open_sockets, CONFIG_LWIP_MAX_SOCKETS);

    const httpd_uri_t routes[] = {
        {.uri = "/api/ping", .method = HTTP_GET, .handler = ping_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/input/status", .method = HTTP_GET,
         .handler = input_status_handler},
        {.uri = "/api/input/select", .method = HTTP_POST,
         .handler = input_select_handler},
        {.uri = "/api/score", .method = HTTP_POST,
         .handler = score_upload_handler},
        {.uri = "/api/scores/sd", .method = HTTP_GET,
         .handler = sd_scores_handler},
        {.uri = "/api/scores/sd/select", .method = HTTP_POST,
         .handler = sd_score_select_handler},
        {.uri = "/api/scores/sd/rename", .method = HTTP_POST,
         .handler = sd_score_rename_handler},
        {.uri = "/api/scores/sd/file", .method = HTTP_GET,
         .handler = sd_score_file_handler},
        {.uri = "/api/practice/preparation", .method = HTTP_GET,
         .handler = preparation_status_handler},
        {.uri = "/api/practice/preparation/select", .method = HTTP_POST,
         .handler = sd_score_select_handler},
        {.uri = "/api/practice/preparation/options", .method = HTTP_POST,
         .handler = preparation_options_handler},
        {.uri = "/api/practice/preparation/start", .method = HTTP_POST,
         .handler = preparation_start_handler},
        {.uri = "/api/practice/restart", .method = HTTP_POST,
         .handler = practice_restart_handler},
        {.uri = "/api/creator/status", .method = HTTP_GET,
         .handler = creator_status_handler},
        {.uri = "/api/creator/start", .method = HTTP_POST,
         .handler = creator_start_handler},
        {.uri = "/api/creator/action", .method = HTTP_POST,
         .handler = creator_action_handler},
        {.uri = "/api/start", .method = HTTP_POST,
         .handler = practice_start_handler},
        {.uri = "/api/stop", .method = HTTP_POST,
         .handler = practice_stop_handler},
        {.uri = "/api/result", .method = HTTP_GET,
         .handler = practice_result_handler},
        {.uri = "/api/practice/advice", .method = HTTP_GET,
         .handler = practice_advice_handler},
        {.uri = "/api/ai/sheet_to_score", .method = HTTP_POST,
         .handler = ai_sheet_to_score_handler},
        {.uri = "/api/ai/score", .method = HTTP_POST,
         .handler = ai_score_handler},
        {.uri = "/api/audio/status", .method = HTTP_GET,
         .handler = audio_status_handler},
        {.uri = "/api/audio/volume", .method = HTTP_POST,
         .handler = volume_handler},
        {.uri = "/api/audio/mute", .method = HTTP_POST,
         .handler = mute_handler},
        {.uri = "/api/audio/tone", .method = HTTP_POST,
         .handler = tone_handler},
        {.uri = "/api/audio/stop", .method = HTTP_POST,
         .handler = stop_handler},
        {.uri = "/api/metronome/start", .method = HTTP_POST,
         .handler = metronome_start_handler},
        {.uri = "/api/metronome/pause", .method = HTTP_POST,
         .handler = metronome_pause_handler},
        {.uri = "/api/metronome/stop", .method = HTTP_POST,
         .handler = metronome_stop_handler},
        {.uri = "/api/audio/files", .method = HTTP_GET,
         .handler = files_handler},
        {.uri = "/api/audio/upload", .method = HTTP_POST,
         .handler = audio_upload_handler},
        {.uri = "/api/audio/file/play", .method = HTTP_POST,
         .handler = file_play_handler},
        {.uri = "/api/audio/file/pause", .method = HTTP_POST,
         .handler = file_pause_handler},
        {.uri = "/api/audio/file/stop", .method = HTTP_POST,
         .handler = file_stop_handler},
        {.uri = "/api/practice/photos", .method = HTTP_GET,
         .handler = practice_photos_handler},
        {.uri = "/api/practice/photo", .method = HTTP_GET,
         .handler = practice_photo_handler},
    };
    for (size_t index = 0; index < sizeof(routes) / sizeof(routes[0]); ++index) {
        err = httpd_register_uri_handler(s_server, &routes[index]);
        if (err != ESP_OK) {
            httpd_stop(s_server);
            s_server = NULL;
            return err;
        }
    }
    ESP_LOGI(TAG, "device and speaker API ready at http://%s", network.ip);
    return ESP_OK;
}

esp_err_t device_api_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}

bool device_api_is_running(void)
{
    return s_server != NULL;
}
