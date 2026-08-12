#include "dashscope_omr.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dashscope_omr_config.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#define OMR_HTTP_TIMEOUT_MS 240000
#define OMR_HTTP_BUFFER_BYTES 4096
#define OMR_RESPONSE_INITIAL_BYTES (16U * 1024U)
#define OMR_RESPONSE_MAX_BYTES (384U * 1024U)
#define OMR_BASE64_INPUT_BYTES 1536U
#define OMR_BASE64_OUTPUT_BYTES (((OMR_BASE64_INPUT_BYTES + 2U) / 3U) * 4U)

static const char *TAG = "DASHSCOPE_OMR";
static SemaphoreHandle_t s_request_lock;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool overflow;
} response_buffer_t;

typedef struct {
    dashscope_omr_error_t error;
    int http_status;
    response_buffer_t response;
} attempt_result_t;

static const char request_prefix[] =
    "{\"model\":\"qwen3.8-max\",\"messages\":[{\"role\":\"user\","
    "\"content\":[{\"type\":\"image_url\",\"image_url\":{\"url\":"
    "\"data:image/jpeg;base64,";

static const char request_after_image[] =
    "\"},\"max_pixels\":8388608},{\"type\":\"text\",\"text\":";

static const char request_suffix[] =
    "}]}],\"response_format\":{\"type\":\"json_object\"},"
    "\"temperature\":0,\"enable_thinking\":false,\"max_tokens\":8000,"
    "\"stream\":false}";

static size_t base64_length(size_t input_length)
{
    return 4U * ((input_length + 2U) / 3U);
}

static size_t escaped_json_length(const char *text)
{
    size_t length = 2U;
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0'; ++cursor) {
        switch (*cursor) {
        case '\\':
        case '"':
        case '\b':
        case '\f':
        case '\n':
        case '\r':
        case '\t':
            length += 2U;
            break;
        default:
            length += *cursor < 0x20U ? 6U : 1U;
            break;
        }
    }
    return length;
}

static char hex_digit(unsigned value)
{
    return value < 10U ? (char)('0' + value) : (char)('a' + value - 10U);
}

static char *escape_json_string(const char *text, size_t *out_length)
{
    size_t capacity = escaped_json_length(text) + 1U;
    char *output = malloc(capacity);
    if (output == NULL) {
        return NULL;
    }
    size_t index = 0;
    output[index++] = '"';
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0'; ++cursor) {
        const char *escape = NULL;
        switch (*cursor) {
        case '\\': escape = "\\\\"; break;
        case '"': escape = "\\\""; break;
        case '\b': escape = "\\b"; break;
        case '\f': escape = "\\f"; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        case '\t': escape = "\\t"; break;
        default: break;
        }
        if (escape != NULL) {
            output[index++] = escape[0];
            output[index++] = escape[1];
        } else if (*cursor < 0x20U) {
            output[index++] = '\\';
            output[index++] = 'u';
            output[index++] = '0';
            output[index++] = '0';
            output[index++] = hex_digit((*cursor >> 4U) & 0x0fU);
            output[index++] = hex_digit(*cursor & 0x0fU);
        } else {
            output[index++] = (char)*cursor;
        }
    }
    output[index++] = '"';
    output[index] = '\0';
    *out_length = index;
    return output;
}

static bool response_reserve(response_buffer_t *buffer, size_t required)
{
    if (required <= buffer->capacity) {
        return true;
    }
    if (required > OMR_RESPONSE_MAX_BYTES) {
        buffer->overflow = true;
        return false;
    }
    size_t capacity = buffer->capacity == 0U ? OMR_RESPONSE_INITIAL_BYTES
                                             : buffer->capacity;
    while (capacity < required && capacity < OMR_RESPONSE_MAX_BYTES) {
        capacity *= 2U;
    }
    if (capacity > OMR_RESPONSE_MAX_BYTES) {
        capacity = OMR_RESPONSE_MAX_BYTES;
    }
    if (capacity < required) {
        buffer->overflow = true;
        return false;
    }
    char *next = realloc(buffer->data, capacity);
    if (next == NULL) {
        return false;
    }
    buffer->data = next;
    buffer->capacity = capacity;
    if (buffer->length == 0U) {
        buffer->data[0] = '\0';
    }
    return true;
}

static bool response_append(response_buffer_t *buffer, const char *data,
                            size_t length)
{
    if (length == 0U) {
        return true;
    }
    if (buffer->length > SIZE_MAX - length - 1U ||
        !response_reserve(buffer, buffer->length + length + 1U)) {
        return false;
    }
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return true;
}

static void response_free(response_buffer_t *buffer)
{
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static dashscope_omr_error_t write_all(esp_http_client_handle_t client,
                                      const char *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        size_t remaining = length - offset;
        int chunk = remaining > INT_MAX ? INT_MAX : (int)remaining;
        int written = esp_http_client_write(client, data + offset, chunk);
        if (written <= 0) {
            return DASHSCOPE_OMR_ERR_NETWORK;
        }
        offset += (size_t)written;
    }
    return DASHSCOPE_OMR_OK;
}

static dashscope_omr_error_t map_transport_error(esp_err_t error)
{
    return error == ESP_ERR_TIMEOUT || error == ESP_ERR_HTTP_EAGAIN
               ? DASHSCOPE_OMR_ERR_TIMEOUT
               : DASHSCOPE_OMR_ERR_NETWORK;
}

static attempt_result_t perform_attempt(const score_capture_t *capture,
                                        const char *escaped_prompt,
                                        size_t escaped_prompt_length,
                                        size_t content_length)
{
    attempt_result_t result = {0};
    result.error = DASHSCOPE_OMR_ERR_NETWORK;
    esp_http_client_config_t config = {
        .url = DASHSCOPE_OMR_ENDPOINT,
        .method = HTTP_METHOD_POST,
        .timeout_ms = OMR_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = OMR_HTTP_BUFFER_BYTES,
        .buffer_size_tx = OMR_HTTP_BUFFER_BYTES,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        result.error = DASHSCOPE_OMR_ERR_NO_MEMORY;
        return result;
    }

    size_t authorization_length = strlen("Bearer ") +
                                  strlen(DASHSCOPE_API_KEY) + 1U;
    char *authorization = malloc(authorization_length);
    if (authorization == NULL) {
        result.error = DASHSCOPE_OMR_ERR_NO_MEMORY;
        esp_http_client_cleanup(client);
        return result;
    }
    snprintf(authorization, authorization_length, "Bearer %s",
             DASHSCOPE_API_KEY);
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");

    esp_err_t open_error =
        esp_http_client_open(client, (int)content_length);
    memset(authorization, 0, authorization_length);
    free(authorization);
    if (open_error != ESP_OK) {
        result.error = map_transport_error(open_error);
        esp_http_client_cleanup(client);
        return result;
    }

    dashscope_omr_error_t error = write_all(
        client, request_prefix, sizeof(request_prefix) - 1U);
    char encoded[OMR_BASE64_OUTPUT_BYTES + 4U];
    size_t offset = 0;
    while (error == DASHSCOPE_OMR_OK && offset < capture->jpeg_length) {
        size_t input_length = capture->jpeg_length - offset;
        if (input_length > OMR_BASE64_INPUT_BYTES) {
            input_length = OMR_BASE64_INPUT_BYTES;
        }
        size_t encoded_length = 0;
        int base64_error = mbedtls_base64_encode(
            (unsigned char *)encoded, sizeof(encoded), &encoded_length,
            capture->jpeg + offset, input_length);
        if (base64_error != 0) {
            error = DASHSCOPE_OMR_ERR_REQUEST_BUILD;
            break;
        }
        error = write_all(client, encoded, encoded_length);
        offset += input_length;
    }
    if (error == DASHSCOPE_OMR_OK) {
        error = write_all(client, request_after_image,
                          sizeof(request_after_image) - 1U);
    }
    if (error == DASHSCOPE_OMR_OK) {
        error = write_all(client, escaped_prompt, escaped_prompt_length);
    }
    if (error == DASHSCOPE_OMR_OK) {
        error = write_all(client, request_suffix,
                          sizeof(request_suffix) - 1U);
    }
    if (error != DASHSCOPE_OMR_OK) {
        result.error = error;
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return result;
    }

    int64_t header_length = esp_http_client_fetch_headers(client);
    if (header_length < 0) {
        result.error = header_length == -ESP_ERR_HTTP_EAGAIN
                           ? DASHSCOPE_OMR_ERR_TIMEOUT
                           : DASHSCOPE_OMR_ERR_NETWORK;
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return result;
    }
    result.http_status = esp_http_client_get_status_code(client);

    char read_buffer[OMR_HTTP_BUFFER_BYTES];
    while (true) {
        int read = esp_http_client_read(client, read_buffer,
                                        sizeof(read_buffer));
        if (read > 0) {
            if (!response_append(&result.response, read_buffer, (size_t)read)) {
                result.error = result.response.overflow
                                   ? DASHSCOPE_OMR_ERR_RESPONSE_TOO_LARGE
                                   : DASHSCOPE_OMR_ERR_NO_MEMORY;
                break;
            }
            continue;
        }
        if (read == 0) {
            result.error = esp_http_client_is_complete_data_received(client)
                               ? dashscope_omr_error_from_http_status(
                                     result.http_status)
                               : DASHSCOPE_OMR_ERR_NETWORK;
            break;
        }
        result.error = read == -ESP_ERR_HTTP_EAGAIN
                           ? DASHSCOPE_OMR_ERR_TIMEOUT
                           : DASHSCOPE_OMR_ERR_NETWORK;
        break;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

bool dashscope_omr_api_key_configured(void)
{
    return DASHSCOPE_API_KEY[0] != '\0' &&
           strcmp(DASHSCOPE_API_KEY, "__DASHSCOPE_API_KEY__") != 0;
}

dashscope_omr_error_t dashscope_omr_recognize(
    const score_capture_t *capture, dashscope_omr_result_t *out_result)
{
    if (out_result != NULL) {
        memset(out_result, 0, sizeof(*out_result));
    }
    if (capture == NULL || capture->jpeg == NULL ||
        capture->jpeg_length == 0U || out_result == NULL) {
        return DASHSCOPE_OMR_ERR_ARGUMENT;
    }
    if (!dashscope_omr_api_key_configured()) {
        out_result->error = DASHSCOPE_OMR_ERR_NOT_CONFIGURED;
        return out_result->error;
    }
    if (s_request_lock == NULL) {
        s_request_lock = xSemaphoreCreateMutex();
        if (s_request_lock == NULL) {
            out_result->error = DASHSCOPE_OMR_ERR_NO_MEMORY;
            return out_result->error;
        }
    }
    if (xSemaphoreTake(s_request_lock, 0) != pdTRUE) {
        out_result->error = DASHSCOPE_OMR_ERR_BUSY;
        return out_result->error;
    }

    int64_t started_us = esp_timer_get_time();
    size_t escaped_prompt_length = 0;
    char *escaped_prompt = escape_json_string(
        dashscope_omr_prompt, &escaped_prompt_length);
    if (escaped_prompt == NULL) {
        out_result->error = DASHSCOPE_OMR_ERR_NO_MEMORY;
        goto done;
    }
    size_t encoded_length = base64_length(capture->jpeg_length);
    if (encoded_length > SIZE_MAX - sizeof(request_prefix) -
                             sizeof(request_after_image) -
                             escaped_prompt_length - sizeof(request_suffix)) {
        out_result->error = DASHSCOPE_OMR_ERR_REQUEST_BUILD;
        goto done;
    }
    size_t content_length = sizeof(request_prefix) - 1U + encoded_length +
                            sizeof(request_after_image) - 1U +
                            escaped_prompt_length +
                            sizeof(request_suffix) - 1U;
    if (content_length > INT_MAX) {
        out_result->error = DASHSCOPE_OMR_ERR_REQUEST_BUILD;
        goto done;
    }

    static const uint32_t retry_delays_ms[] = {2000U, 5000U};
    for (size_t attempt = 0; attempt < 3U; ++attempt) {
        attempt_result_t attempt_result = perform_attempt(
            capture, escaped_prompt, escaped_prompt_length, content_length);
        out_result->attempt_count = (uint8_t)(attempt + 1U);
        out_result->http_status = attempt_result.http_status;
        out_result->error = attempt_result.error;

        if (attempt_result.error == DASHSCOPE_OMR_OK) {
            dashscope_omr_parsed_response_t parsed = {0};
            out_result->error = dashscope_omr_parse_completion(
                attempt_result.response.data, attempt_result.response.length,
                &parsed);
            if (out_result->error == DASHSCOPE_OMR_OK) {
                out_result->compact_json = parsed.compact_json;
                out_result->score = parsed.score;
                out_result->usage = parsed.usage;
                out_result->score_error = parsed.score_error;
                memset(&parsed, 0, sizeof(parsed));
            } else {
                out_result->score_error = parsed.score_error;
            }
            dashscope_omr_parsed_response_free(&parsed);
        }
        bool should_retry = attempt < 2U &&
                            dashscope_omr_should_retry(
                                attempt_result.error,
                                attempt_result.http_status);
        response_free(&attempt_result.response);
        if (out_result->error == DASHSCOPE_OMR_OK || !should_retry) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(retry_delays_ms[attempt]));
    }

done:
    free(escaped_prompt);
    int64_t elapsed_us = esp_timer_get_time() - started_us;
    out_result->elapsed_ms = elapsed_us > 0
                                 ? (uint32_t)(elapsed_us / 1000LL)
                                 : 0U;
    ESP_LOGI(TAG,
             "OMR task=%s jpeg=%u elapsed_ms=%lu http=%d attempts=%u "
             "input_tokens=%lu output_tokens=%lu events=%u result=%s",
             capture->task_id, (unsigned)capture->jpeg_length,
             (unsigned long)out_result->elapsed_ms, out_result->http_status,
             (unsigned)out_result->attempt_count,
             (unsigned long)out_result->usage.input_tokens,
             (unsigned long)out_result->usage.output_tokens,
             out_result->score != NULL ? (unsigned)out_result->score->event_count
                                       : 0U,
             dashscope_omr_error_name(out_result->error));
    xSemaphoreGive(s_request_lock);
    return out_result->error;
}

void dashscope_omr_result_free(dashscope_omr_result_t *result)
{
    if (result == NULL) {
        return;
    }
    free(result->compact_json);
    compact_score_free(result->score);
    memset(result, 0, sizeof(*result));
}
