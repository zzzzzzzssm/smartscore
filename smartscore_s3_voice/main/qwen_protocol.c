#include "qwen_protocol.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "mbedtls/base64.h"

static bool find_string(const char *json,
                        size_t length,
                        const char *key,
                        bool top_level,
                        qwen_json_span_t *span)
{
    span->data = NULL;
    span->length = 0U;
    const size_t key_length = strlen(key);
    unsigned depth = 0U;
    for (size_t cursor = 0U; cursor < length;) {
        char byte = json[cursor];
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
        const size_t start = ++cursor;
        bool escaped = false;
        while (cursor < length) {
            byte = json[cursor];
            if (!escaped && byte == '"') break;
            escaped = !escaped && byte == '\\';
            ++cursor;
        }
        if (cursor >= length) return false;
        const size_t string_length = cursor - start;
        ++cursor;
        if ((top_level && depth != 1U) || string_length != key_length ||
            memcmp(json + start, key, key_length) != 0) {
            continue;
        }
        while (cursor < length &&
               (json[cursor] == ' ' || json[cursor] == '\t' ||
                json[cursor] == '\r' || json[cursor] == '\n')) ++cursor;
        if (cursor >= length || json[cursor++] != ':') continue;
        while (cursor < length &&
               (json[cursor] == ' ' || json[cursor] == '\t' ||
                json[cursor] == '\r' || json[cursor] == '\n')) ++cursor;
        if (cursor >= length || json[cursor++] != '"') continue;
        const size_t value_start = cursor;
        escaped = false;
        while (cursor < length) {
            byte = json[cursor];
            if (!escaped && byte == '"') {
                span->data = json + value_start;
                span->length = cursor - value_start;
                return true;
            }
            escaped = !escaped && byte == '\\';
            ++cursor;
        }
        return false;
    }
    return false;
}

static void copy_span(char *output, size_t capacity,
                      const qwen_json_span_t *span)
{
    output[0] = '\0';
    if (span->data == NULL || capacity == 0U) return;
    size_t copy = span->length < capacity - 1U ? span->length : capacity - 1U;
    memcpy(output, span->data, copy);
    output[copy] = '\0';
}

static qwen_server_kind_t classify(const char *type)
{
    if (strcmp(type, "session.created") == 0) return QWEN_SERVER_SESSION_CREATED;
    if (strcmp(type, "session.updated") == 0) return QWEN_SERVER_SESSION_UPDATED;
    if (strcmp(type, "input_audio_buffer.committed") == 0) return QWEN_SERVER_INPUT_COMMITTED;
    if (strcmp(type, "input_audio_buffer.cleared") == 0) return QWEN_SERVER_INPUT_CLEARED;
    if (strcmp(type, "input_audio_buffer.speech_started") == 0 ||
        strcmp(type, "conversation.item.input_audio_transcription.started") == 0)
        return QWEN_SERVER_ASR_STARTED;
    if (strcmp(type, "conversation.item.input_audio_transcription.completed") == 0)
        return QWEN_SERVER_ASR_COMPLETED;
    if (strcmp(type, "response.audio_transcript.delta") == 0 ||
        strcmp(type, "response.text.delta") == 0) return QWEN_SERVER_TEXT_DELTA;
    if (strcmp(type, "response.audio_transcript.done") == 0 ||
        strcmp(type, "response.text.done") == 0) return QWEN_SERVER_TEXT_DONE;
    if (strcmp(type, "response.audio.delta") == 0) return QWEN_SERVER_AUDIO_DELTA;
    if (strcmp(type, "response.audio.done") == 0) return QWEN_SERVER_AUDIO_DONE;
    if (strcmp(type, "response.done") == 0) return QWEN_SERVER_RESPONSE_DONE;
    if (strcmp(type, "response.canceled") == 0) return QWEN_SERVER_RESPONSE_CANCELED;
    if (strcmp(type, "session.closed") == 0) return QWEN_SERVER_SESSION_CLOSED;
    if (strcmp(type, "error") == 0 ||
        strcmp(type, "conversation.item.input_audio_transcription.failed") == 0)
        return QWEN_SERVER_ERROR;
    if (strncmp(type, "response.", 9U) == 0 ||
        strncmp(type, "conversation.", 13U) == 0 ||
        strcmp(type, "rate_limits.updated") == 0 ||
        strcmp(type, "input_audio_buffer.speech_stopped") == 0)
        return QWEN_SERVER_IGNORED;
    return QWEN_SERVER_UNKNOWN;
}

esp_err_t qwen_protocol_parse_server_event(const char *json,
                                            size_t json_length,
                                            qwen_server_event_t *event)
{
    if (json == NULL || json_length == 0U || event == NULL)
        return ESP_ERR_INVALID_ARG;
    memset(event, 0, sizeof(*event));
    qwen_json_span_t type = {0};
    if (!find_string(json, json_length, "type", true, &type))
        return ESP_ERR_INVALID_RESPONSE;
    copy_span(event->type, sizeof(event->type), &type);
    event->kind = classify(event->type);
    (void)find_string(json, json_length, "delta", true, &event->delta);
    (void)find_string(json, json_length, "transcript", true,
                      &event->transcript);
    (void)find_string(json, json_length, "text", true, &event->text);
    (void)find_string(json, json_length, "code", false, &event->error_code);
    (void)find_string(json, json_length, "message", false,
                      &event->error_message);
    return ESP_OK;
}

esp_err_t qwen_protocol_build_session_update(char *output,
                                              size_t capacity,
                                              const char *voice,
                                              const char *instructions,
                                              size_t *output_length)
{
    if (output == NULL || voice == NULL || instructions == NULL ||
        output_length == NULL) return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_CreateObject();
    cJSON *session = cJSON_CreateObject();
    cJSON *modalities = cJSON_CreateArray();
    if (root == NULL || session == NULL || modalities == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(session);
        cJSON_Delete(modalities);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON_AddStringToObject(session, "instructions", instructions);
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
    cJSON_AddItemToObject(session, "modalities", modalities);
    cJSON_AddStringToObject(session, "voice", voice);
    cJSON_AddStringToObject(session, "input_audio_format", "pcm");
    cJSON_AddStringToObject(session, "output_audio_format", "pcm");
    cJSON_AddNumberToObject(session, "max_history_turns", 5);
    cJSON_AddNullToObject(session, "turn_detection");
    cJSON_AddItemToObject(root, "session", session);
    if (!cJSON_PrintPreallocated(root, output, (int)capacity, false)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }
    *output_length = strlen(output);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t qwen_protocol_build_audio_append(char *output,
                                            size_t capacity,
                                            const uint8_t *pcm,
                                            size_t pcm_bytes,
                                            size_t *output_length)
{
    static const char prefix[] =
        "{\"type\":\"input_audio_buffer.append\",\"audio\":\"";
    static const char suffix[] = "\"}";
    if (output == NULL || pcm == NULL || pcm_bytes == 0U ||
        output_length == NULL) return ESP_ERR_INVALID_ARG;
    const size_t encoded = ((pcm_bytes + 2U) / 3U) * 4U;
    if (sizeof(prefix) - 1U + encoded + sizeof(suffix) > capacity)
        return ESP_ERR_INVALID_SIZE;
    memcpy(output, prefix, sizeof(prefix) - 1U);
    size_t actual = 0U;
    if (mbedtls_base64_encode((unsigned char *)output + sizeof(prefix) - 1U,
                              capacity - sizeof(prefix) + 1U, &actual,
                              pcm, pcm_bytes) != 0) return ESP_FAIL;
    size_t cursor = sizeof(prefix) - 1U + actual;
    memcpy(output + cursor, suffix, sizeof(suffix));
    *output_length = cursor + sizeof(suffix) - 1U;
    return ESP_OK;
}

esp_err_t qwen_protocol_build_simple_event(char *output,
                                            size_t capacity,
                                            const char *type,
                                            size_t *output_length)
{
    if (output == NULL || type == NULL || output_length == NULL)
        return ESP_ERR_INVALID_ARG;
    int count = snprintf(output, capacity, "{\"type\":\"%s\"}", type);
    if (count <= 0 || (size_t)count >= capacity) return ESP_ERR_INVALID_SIZE;
    *output_length = (size_t)count;
    return ESP_OK;
}

esp_err_t qwen_protocol_decode_audio(const qwen_json_span_t *encoded,
                                      uint8_t *output,
                                      size_t capacity,
                                      size_t *output_length)
{
    if (encoded == NULL || encoded->data == NULL || encoded->length == 0U ||
        (encoded->length & 3U) != 0U || output == NULL || output_length == NULL)
        return ESP_ERR_INVALID_ARG;
    if (mbedtls_base64_decode(output, capacity, output_length,
                              (const unsigned char *)encoded->data,
                              encoded->length) != 0 ||
        ((*output_length) & 1U) != 0U) return ESP_FAIL;
    return ESP_OK;
}
