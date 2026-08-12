#include "voice_v2_protocol.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "mbedtls/base64.h"

static bool find_json_string(const char *json,
                             size_t json_length,
                             const char *key,
                             voice_v2_json_span_t *out_span)
{
    if (json == NULL || key == NULL || out_span == NULL) return false;
    out_span->data = NULL;
    out_span->length = 0U;

    const size_t key_length = strlen(key);
    for (size_t i = 0U; i + key_length + 2U < json_length; ++i) {
        if (json[i] != '"' ||
            i + key_length + 1U >= json_length ||
            memcmp(json + i + 1U, key, key_length) != 0 ||
            json[i + key_length + 1U] != '"') {
            continue;
        }
        size_t cursor = i + key_length + 2U;
        while (cursor < json_length &&
               (json[cursor] == ' ' || json[cursor] == '\t' ||
                json[cursor] == '\r' || json[cursor] == '\n')) {
            ++cursor;
        }
        if (cursor >= json_length || json[cursor++] != ':') continue;
        while (cursor < json_length &&
               (json[cursor] == ' ' || json[cursor] == '\t' ||
                json[cursor] == '\r' || json[cursor] == '\n')) {
            ++cursor;
        }
        if (cursor >= json_length || json[cursor++] != '"') continue;

        const size_t value_start = cursor;
        bool escaped = false;
        while (cursor < json_length) {
            const char byte = json[cursor];
            if (!escaped && byte == '"') {
                out_span->data = json + value_start;
                out_span->length = cursor - value_start;
                return true;
            }
            if (!escaped && byte == '\\') {
                escaped = true;
            } else {
                escaped = false;
            }
            ++cursor;
        }
        return false;
    }
    return false;
}

static bool find_top_level_json_string(const char *json,
                                       size_t json_length,
                                       const char *key,
                                       voice_v2_json_span_t *out_span)
{
    if (json == NULL || key == NULL || out_span == NULL) return false;
    out_span->data = NULL;
    out_span->length = 0U;
    const size_t key_length = strlen(key);
    unsigned depth = 0U;

    for (size_t cursor = 0U; cursor < json_length;) {
        const char byte = json[cursor];
        if (byte == '{' || byte == '[') {
            ++depth;
            ++cursor;
            continue;
        }
        if (byte == '}' || byte == ']') {
            if (depth > 0U) --depth;
            ++cursor;
            continue;
        }
        if (byte != '"') {
            ++cursor;
            continue;
        }

        const size_t string_start = ++cursor;
        bool escaped = false;
        while (cursor < json_length) {
            const char string_byte = json[cursor];
            if (!escaped && string_byte == '"') break;
            if (!escaped && string_byte == '\\') escaped = true;
            else escaped = false;
            ++cursor;
        }
        if (cursor >= json_length) return false;
        const size_t string_length = cursor - string_start;
        ++cursor;
        if (depth != 1U || string_length != key_length ||
            memcmp(json + string_start, key, key_length) != 0) {
            continue;
        }
        while (cursor < json_length &&
               (json[cursor] == ' ' || json[cursor] == '\t' ||
                json[cursor] == '\r' || json[cursor] == '\n')) {
            ++cursor;
        }
        if (cursor >= json_length || json[cursor++] != ':') continue;
        while (cursor < json_length &&
               (json[cursor] == ' ' || json[cursor] == '\t' ||
                json[cursor] == '\r' || json[cursor] == '\n')) {
            ++cursor;
        }
        if (cursor >= json_length || json[cursor++] != '"') continue;
        const size_t value_start = cursor;
        escaped = false;
        while (cursor < json_length) {
            const char value_byte = json[cursor];
            if (!escaped && value_byte == '"') {
                out_span->data = json + value_start;
                out_span->length = cursor - value_start;
                return true;
            }
            if (!escaped && value_byte == '\\') escaped = true;
            else escaped = false;
            ++cursor;
        }
        return false;
    }
    return false;
}

static void copy_span(char *output,
                      size_t output_capacity,
                      const voice_v2_json_span_t *span)
{
    if (output == NULL || output_capacity == 0U) return;
    output[0] = '\0';
    if (span == NULL || span->data == NULL) return;
    size_t copy = span->length;
    if (copy >= output_capacity) copy = output_capacity - 1U;
    memcpy(output, span->data, copy);
    output[copy] = '\0';
}

static voice_v2_server_kind_t classify_event(const char *type)
{
    if (strcmp(type, "session.created") == 0)
        return VOICE_V2_SERVER_SESSION_CREATED;
    if (strcmp(type, "session.updated") == 0)
        return VOICE_V2_SERVER_SESSION_UPDATED;
    if (strcmp(type, "input_audio_buffer.committed") == 0)
        return VOICE_V2_SERVER_INPUT_COMMITTED;
    if (strcmp(type, "input_audio_buffer.cleared") == 0)
        return VOICE_V2_SERVER_INPUT_CLEARED;
    if (strcmp(type, "input_audio_buffer.speech_started") == 0)
        return VOICE_V2_SERVER_ASR_STARTED;
    if (strcmp(type, "input_audio_buffer.speech_stopped") == 0)
        return VOICE_V2_SERVER_IGNORED;
    if (strcmp(type,
               "conversation.item.input_audio_transcription.started") == 0)
        return VOICE_V2_SERVER_ASR_STARTED;
    if (strcmp(type,
               "conversation.item.input_audio_transcription.delta") == 0)
        return VOICE_V2_SERVER_ASR_DELTA;
    if (strcmp(type,
               "conversation.item.input_audio_transcription.completed") == 0)
        return VOICE_V2_SERVER_ASR_COMPLETED;
    if (strcmp(type,
               "conversation.item.input_audio_transcription.failed") == 0)
        return VOICE_V2_SERVER_ASR_FAILED;
    if (strcmp(type, "response.audio_transcript.delta") == 0 ||
        strcmp(type, "response.text.delta") == 0)
        return VOICE_V2_SERVER_TEXT_DELTA;
    if (strcmp(type, "response.audio_transcript.done") == 0 ||
        strcmp(type, "response.text.done") == 0)
        return VOICE_V2_SERVER_TEXT_DONE;
    if (strcmp(type, "response.audio.delta") == 0)
        return VOICE_V2_SERVER_AUDIO_DELTA;
    if (strcmp(type, "response.audio.done") == 0)
        return VOICE_V2_SERVER_AUDIO_DONE;
    if (strcmp(type, "response.done") == 0)
        return VOICE_V2_SERVER_RESPONSE_DONE;
    if (strcmp(type, "response.canceled") == 0)
        return VOICE_V2_SERVER_RESPONSE_CANCELED;
    if (strcmp(type, "session.closed") == 0)
        return VOICE_V2_SERVER_SESSION_CLOSED;
    if (strcmp(type, "conversation.item.created") == 0 ||
        strcmp(type, "response.created") == 0 ||
        strcmp(type, "response.output_item.added") == 0 ||
        strcmp(type, "response.content_part.added") == 0 ||
        strcmp(type, "response.content_part.done") == 0 ||
        strcmp(type, "response.output_item.done") == 0 ||
        strcmp(type, "rate_limits.updated") == 0) {
        return VOICE_V2_SERVER_IGNORED;
    }
    if (strcmp(type, "error") == 0)
        return VOICE_V2_SERVER_ERROR;
    return VOICE_V2_SERVER_UNKNOWN;
}

esp_err_t voice_v2_protocol_parse_server_event(
    const char *json,
    size_t json_length,
    voice_v2_server_event_t *out_event)
{
    if (json == NULL || json_length == 0U || out_event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_event, 0, sizeof(*out_event));
    voice_v2_json_span_t type = {0};
    if (!find_top_level_json_string(json, json_length, "type", &type)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    copy_span(out_event->type, sizeof(out_event->type), &type);
    out_event->kind = classify_event(out_event->type);
    (void)find_top_level_json_string(json, json_length, "event_id",
                                     &out_event->event_id);
    (void)find_top_level_json_string(json, json_length, "session_id",
                                     &out_event->session_id);
    (void)find_top_level_json_string(json, json_length, "delta",
                                     &out_event->delta);
    (void)find_top_level_json_string(json, json_length, "transcript",
                                     &out_event->transcript);
    (void)find_top_level_json_string(json, json_length, "text",
                                     &out_event->text);
    (void)find_json_string(json, json_length, "status",
                           &out_event->response_status);
    (void)find_json_string(json, json_length, "code",
                           &out_event->error_code);
    (void)find_json_string(json, json_length, "message",
                           &out_event->error_message);
    return ESP_OK;
}

static esp_err_t add_session_json(cJSON *root,
                                  const char *voice,
                                  const char *instructions,
                                  bool include_static_configuration)
{
    cJSON *session = cJSON_CreateObject();
    if (session == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(session, "instructions", instructions);
    if (include_static_configuration) {
        cJSON *modalities = cJSON_CreateArray();
        if (modalities == NULL) {
            cJSON_Delete(session);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
        cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
        cJSON_AddItemToObject(session, "modalities", modalities);
        cJSON_AddStringToObject(session, "voice", voice);
        cJSON_AddStringToObject(session, "input_audio_format", "pcm");
        cJSON_AddStringToObject(session, "output_audio_format", "pcm");
        cJSON_AddNumberToObject(session, "max_history_turns", 5);
        cJSON_AddNullToObject(session, "turn_detection");
    }
    cJSON_AddItemToObject(root, "session", session);
    return ESP_OK;
}

esp_err_t voice_v2_protocol_build_session_update(
    char *output,
    size_t output_capacity,
    const char *voice,
    const char *instructions,
    bool include_static_configuration,
    size_t *out_length)
{
    if (output == NULL || output_capacity == 0U || voice == NULL ||
        instructions == NULL || out_length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_length = 0U;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "type", "session.update");
    esp_err_t err = add_session_json(root, voice, instructions,
                                     include_static_configuration);
    if (err == ESP_OK &&
        !cJSON_PrintPreallocated(root, output, (int)output_capacity, false)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) *out_length = strlen(output);
    cJSON_Delete(root);
    return err;
}

esp_err_t voice_v2_protocol_build_audio_append(
    char *output,
    size_t output_capacity,
    const uint8_t *pcm,
    size_t pcm_bytes,
    size_t *out_length)
{
    static const char prefix[] =
        "{\"type\":\"input_audio_buffer.append\",\"audio\":\"";
    static const char suffix[] = "\"}";
    if (output == NULL || pcm == NULL || pcm_bytes == 0U ||
        out_length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_length = 0U;
    const size_t encoded_capacity = ((pcm_bytes + 2U) / 3U) * 4U;
    const size_t required = sizeof(prefix) - 1U + encoded_capacity +
                            sizeof(suffix);
    if (required > output_capacity) return ESP_ERR_INVALID_SIZE;
    memcpy(output, prefix, sizeof(prefix) - 1U);
    size_t encoded_length = 0U;
    int rc = mbedtls_base64_encode(
        (unsigned char *)output + sizeof(prefix) - 1U,
        output_capacity - (sizeof(prefix) - 1U), &encoded_length,
        pcm, pcm_bytes);
    if (rc != 0) return ESP_FAIL;
    size_t cursor = sizeof(prefix) - 1U + encoded_length;
    memcpy(output + cursor, suffix, sizeof(suffix));
    cursor += sizeof(suffix) - 1U;
    *out_length = cursor;
    return ESP_OK;
}

esp_err_t voice_v2_protocol_build_simple_event(
    char *output,
    size_t output_capacity,
    const char *type,
    size_t *out_length)
{
    if (output == NULL || type == NULL || out_length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int length = snprintf(output, output_capacity, "{\"type\":\"%s\"}",
                          type);
    if (length <= 0 || (size_t)length >= output_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    *out_length = (size_t)length;
    return ESP_OK;
}

esp_err_t voice_v2_protocol_decode_audio_span(
    const voice_v2_json_span_t *encoded,
    uint8_t *output,
    size_t output_capacity,
    size_t *out_length)
{
    if (encoded == NULL || encoded->data == NULL || encoded->length == 0U ||
        output == NULL || out_length == NULL ||
        (encoded->length & 3U) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_length = 0U;
    int rc = mbedtls_base64_decode(
        output, output_capacity, out_length,
        (const unsigned char *)encoded->data, encoded->length);
    if (rc != 0 || ((*out_length) & 1U) != 0U) return ESP_FAIL;
    return ESP_OK;
}
