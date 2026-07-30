#include "doubao_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

#ifndef VEI_API_KEY
#define VEI_API_KEY ""
#endif

#ifndef DOUBAO_API_URL
#define DOUBAO_API_URL "https://ai-gateway.vei.volces.com/v1/chat/completions"
#endif

#ifndef DOUBAO_TEXT_MODEL
#define DOUBAO_TEXT_MODEL "doubao-seed-1.6-vision"
#endif

#ifndef DOUBAO_VISION_MODEL
#define DOUBAO_VISION_MODEL "doubao-seed-1.6-vision"
#endif

#define DOUBAO_MAX_RESPONSE_BYTES (256 * 1024)
#define DOUBAO_RESPONSE_INITIAL_CAP (32 * 1024)
#define DOUBAO_HTTP_BUFFER_BYTES 4096
#define DOUBAO_HTTP_TIMEOUT_MS 300000
#define DOUBAO_BASE64_CHUNK_BYTES 1536
#define DOUBAO_BASE64_CHUNK_ENCODED_BYTES (((DOUBAO_BASE64_CHUNK_BYTES + 2) / 3) * 4)

static const char *TAG = "doubao";

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool truncated;
} response_buffer_t;

static char *doubao_heap_alloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }
#ifdef CONFIG_SPIRAM
    char *psram = (char *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (psram != NULL) {
        return psram;
    }
#endif
    char *heap = (char *)heap_caps_malloc(size, MALLOC_CAP_8BIT);
    return heap != NULL ? heap : (char *)malloc(size);
}

static char *doubao_heap_grow(char *old_data, size_t old_len, size_t new_size)
{
    char *next = doubao_heap_alloc(new_size);
    if (next == NULL) {
        return NULL;
    }
    if (old_data != NULL && old_len > 0) {
        memcpy(next, old_data, old_len);
    }
    free(old_data);
    return next;
}

static esp_err_t reserve_response_data(response_buffer_t *buffer, size_t needed)
{
    if (buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (needed <= buffer->cap) {
        return ESP_OK;
    }

    size_t next_cap = buffer->cap == 0 ? DOUBAO_RESPONSE_INITIAL_CAP : buffer->cap;
    while (next_cap < needed && next_cap < DOUBAO_MAX_RESPONSE_BYTES) {
        next_cap *= 2;
    }
    if (next_cap > DOUBAO_MAX_RESPONSE_BYTES) {
        next_cap = DOUBAO_MAX_RESPONSE_BYTES;
    }
    if (next_cap < needed) {
        buffer->truncated = true;
        return ESP_ERR_NO_MEM;
    }

    char *next = doubao_heap_grow(buffer->data, buffer->len + 1, next_cap);
    if (next == NULL) {
        ESP_LOGW(TAG,
                 "doubao response grow failed: len=%u cap=%u requested=%u max=%u",
                 (unsigned)buffer->len,
                 (unsigned)buffer->cap,
                 (unsigned)next_cap,
                 (unsigned)DOUBAO_MAX_RESPONSE_BYTES);
        return ESP_ERR_NO_MEM;
    }
    buffer->data = next;
    buffer->cap = next_cap;
    if (buffer->len == 0) {
        buffer->data[0] = '\0';
    }
    return ESP_OK;
}

static char *dup_string(const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (copy != NULL) {
        memcpy(copy, s, len);
    }
    return copy;
}

static char *dup_range(const char *start, size_t len)
{
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, start, len);
    copy[len] = '\0';
    return copy;
}

static char *build_error_json(const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return dup_string("{\"ok\":false,\"message\":\"out of memory\"}");
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "message", message != NULL ? message : "doubao error");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json != NULL ? json : dup_string("{\"ok\":false,\"message\":\"failed to build error\"}");
}

static void log_response_preview(const char *label, const char *text)
{
    if (text == NULL) {
        ESP_LOGW(TAG, "%s: <null>", label != NULL ? label : "response preview");
        return;
    }

    char preview[321];
    size_t i = 0;
    for (; i < sizeof(preview) - 1 && text[i] != '\0'; ++i) {
        unsigned char ch = (unsigned char)text[i];
        preview[i] = (ch >= 0x20 && ch <= 0x7e) ? (char)ch : '.';
    }
    preview[i] = '\0';
    ESP_LOGW(TAG,
             "%s first %u chars: %s",
             label != NULL ? label : "response preview",
             (unsigned)i,
             preview);
}

static const char *skip_json_prefix_noise(const char *text)
{
    if (text == NULL) {
        return NULL;
    }
    const unsigned char *p = (const unsigned char *)text;
    if (p[0] == 0xef && p[1] == 0xbb && p[2] == 0xbf) {
        p += 3;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    return (const char *)p;
}

static size_t json_escaped_len(const char *text)
{
    size_t len = 0;
    const char *p = text != NULL ? text : "";
    while (*p != '\0') {
        unsigned char ch = (unsigned char)*p++;
        switch (ch) {
        case '\\':
        case '"':
        case '\n':
        case '\r':
        case '\t':
            len += 2;
            break;
        default:
            len += ch < 0x20 ? 6 : 1;
            break;
        }
    }
    return len;
}

static bool append_raw_to_buffer(char *buffer, size_t cap, size_t *len, const char *text)
{
    if (buffer == NULL || len == NULL || text == NULL) {
        return false;
    }
    size_t text_len = strlen(text);
    if (*len + text_len >= cap) {
        return false;
    }
    memcpy(buffer + *len, text, text_len);
    *len += text_len;
    buffer[*len] = '\0';
    return true;
}

static bool append_escaped_json_string(char *buffer, size_t cap, size_t *len, const char *text)
{
    if (!append_raw_to_buffer(buffer, cap, len, "\"")) {
        return false;
    }
    const char *p = text != NULL ? text : "";
    while (*p != '\0') {
        unsigned char ch = (unsigned char)*p++;
        const char *escaped = NULL;
        char unicode[7];
        switch (ch) {
        case '\\':
            escaped = "\\\\";
            break;
        case '"':
            escaped = "\\\"";
            break;
        case '\n':
            escaped = "\\n";
            break;
        case '\r':
            escaped = "\\r";
            break;
        case '\t':
            escaped = "\\t";
            break;
        default:
            if (ch < 0x20) {
                snprintf(unicode, sizeof(unicode), "\\u%04x", ch);
                escaped = unicode;
            }
            break;
        }
        if (escaped != NULL) {
            if (!append_raw_to_buffer(buffer, cap, len, escaped)) {
                return false;
            }
        } else {
            if (*len + 1 >= cap) {
                return false;
            }
            buffer[(*len)++] = (char)ch;
            buffer[*len] = '\0';
        }
    }
    return append_raw_to_buffer(buffer, cap, len, "\"");
}

static const char *get_api_key(void)
{
    const char *env = getenv("VEI_API_KEY");
    if (env != NULL && env[0] != '\0') {
        return env;
    }
    return VEI_API_KEY;
}

bool doubao_api_key_configured(void)
{
    const char *key = get_api_key();
    return key != NULL && key[0] != '\0';
}

static esp_err_t append_response_data(response_buffer_t *buffer, const char *data, int data_len)
{
    if (buffer == NULL || data == NULL || data_len <= 0) {
        return ESP_OK;
    }

    size_t incoming = (size_t)data_len;
    if (buffer->len + incoming >= DOUBAO_MAX_RESPONSE_BYTES) {
        incoming = DOUBAO_MAX_RESPONSE_BYTES > buffer->len ? DOUBAO_MAX_RESPONSE_BYTES - buffer->len - 1 : 0;
        buffer->truncated = true;
    }
    if (incoming == 0) {
        return ESP_OK;
    }

    size_t needed = buffer->len + incoming + 1;
    if (reserve_response_data(buffer, needed) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(buffer->data + buffer->len, data, incoming);
    buffer->len += incoming;
    buffer->data[buffer->len] = '\0';
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        return append_response_data((response_buffer_t *)evt->user_data,
                                    (const char *)evt->data,
                                    evt->data_len);
    }
    return ESP_OK;
}

static char *post_json_to_doubao(const char *body)
{
    if (!doubao_api_key_configured()) {
        return build_error_json("missing VEI_API_KEY; set it before building firmware");
    }
    if (body == NULL) {
        return build_error_json("empty doubao request");
    }

    response_buffer_t response = { 0 };
    if (reserve_response_data(&response, DOUBAO_RESPONSE_INITIAL_CAP) != ESP_OK) {
        return build_error_json("not enough memory for doubao response buffer");
    }
    esp_http_client_config_t config = {
        .url = DOUBAO_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = DOUBAO_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = DOUBAO_HTTP_BUFFER_BYTES,
        .buffer_size_tx = DOUBAO_HTTP_BUFFER_BYTES,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(response.data);
        return build_error_json("failed to init http client");
    }

    const char *key = get_api_key();
    size_t auth_len = strlen("Bearer ") + strlen(key) + 1;
    char *auth = malloc(auth_len);
    if (auth == NULL) {
        free(response.data);
        esp_http_client_cleanup(client);
        return build_error_json("not enough memory for authorization");
    }
    snprintf(auth, auth_len, "Bearer %s", key);

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "doubao http status=%d err=%s response_bytes=%u",
             status,
             esp_err_to_name(err),
             (unsigned)response.len);

    free(auth);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        free(response.data);
        return build_error_json(esp_err_to_name(err));
    }
    if (response.truncated) {
        free(response.data);
        return build_error_json("doubao response too large");
    }
    if (status < 200 || status >= 300) {
        log_response_preview("doubao error response", response.data);
        char message[96];
        snprintf(message, sizeof(message), "doubao http status %d", status);
        char *error = build_error_json(message);
        free(response.data);
        return error;
    }
    if (response.data == NULL) {
        return build_error_json("empty doubao response");
    }
    return response.data;
}

static size_t base64_encoded_len(size_t input_len)
{
    return ((input_len + 2) / 3) * 4;
}

static esp_err_t client_write_all(esp_http_client_handle_t client, const char *data, size_t len)
{
    size_t written = 0;
    while (written < len) {
        int ret = esp_http_client_write(client, data + written, (int)(len - written));
        if (ret <= 0) {
            return ESP_FAIL;
        }
        written += (size_t)ret;
    }
    return ESP_OK;
}

static char *finish_streamed_doubao_request(esp_http_client_handle_t client,
                                            response_buffer_t *response)
{
    int header_ret = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "doubao streamed status=%d header_ret=%d", status, header_ret);

    if (header_ret == -ESP_ERR_HTTP_EAGAIN || header_ret == ESP_ERR_HTTP_EAGAIN) {
        esp_http_client_close(client);
        return build_error_json("doubao response timed out; try a smaller or clearer image");
    }
    if (header_ret < 0) {
        esp_http_client_close(client);
        return build_error_json("failed to fetch doubao response headers");
    }

    char read_buf[2048];
    while (1) {
        int ret = esp_http_client_read(client, read_buf, sizeof(read_buf));
        if (ret > 0) {
            esp_err_t append_err = append_response_data(response, read_buf, ret);
            if (append_err != ESP_OK) {
                char message[128];
                snprintf(message,
                         sizeof(message),
                         "not enough memory for doubao response, len=%u cap=%u",
                         (unsigned)response->len,
                         (unsigned)response->cap);
                esp_http_client_close(client);
                free(response->data);
                return build_error_json(message);
            }
            continue;
        }
        if (ret == 0) {
            break;
        }
        esp_http_client_close(client);
        free(response->data);
        return build_error_json("failed to read doubao response");
    }

    esp_http_client_close(client);
    ESP_LOGI(TAG,
             "doubao streamed response_bytes=%u truncated=%d",
             (unsigned)response->len,
             response->truncated ? 1 : 0);

    if (response->truncated) {
        free(response->data);
        return build_error_json("doubao response too large");
    }
    if (status < 200 || status >= 300) {
        log_response_preview("doubao streamed error response", response->data);
        char message[96];
        snprintf(message, sizeof(message), "doubao http status %d", status);
        char *error = build_error_json(message);
        free(response->data);
        return error;
    }
    if (response->data == NULL) {
        return build_error_json("empty doubao response");
    }
    return response->data;
}

static char *post_image_request_to_doubao(const uint8_t *image,
                                          size_t image_len,
                                          const char *mime_type,
                                          const char *prompt)
{
    if (!doubao_api_key_configured()) {
        return build_error_json("missing VEI_API_KEY; set it before building firmware");
    }

    response_buffer_t response = { 0 };
    if (reserve_response_data(&response, DOUBAO_RESPONSE_INITIAL_CAP) != ESP_OK) {
        return build_error_json("not enough memory for doubao response buffer");
    }
    esp_http_client_config_t config = {
        .url = DOUBAO_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = DOUBAO_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = DOUBAO_HTTP_BUFFER_BYTES,
        .buffer_size_tx = DOUBAO_HTTP_BUFFER_BYTES,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(response.data);
        return build_error_json("failed to init http client");
    }

    const char *key = get_api_key();
    size_t auth_len = strlen("Bearer ") + strlen(key) + 1;
    char *auth = malloc(auth_len);
    if (auth == NULL) {
        free(response.data);
        esp_http_client_cleanup(client);
        return build_error_json("not enough memory for authorization");
    }
    snprintf(auth, auth_len, "Bearer %s", key);

    char prefix[512];
    int prefix_len = snprintf(prefix,
                              sizeof(prefix),
                              "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":["
                              "{\"type\":\"text\",\"text\":",
                              DOUBAO_VISION_MODEL);
    char image_prefix[192];
    int image_prefix_len = snprintf(image_prefix,
                                    sizeof(image_prefix),
                                    "},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:%s;base64,",
                                    mime_type);
    const char *suffix = "\"}}]}],\"temperature\":0.0,\"max_tokens\":4096}";
    if (prefix_len <= 0 || prefix_len >= (int)sizeof(prefix) ||
        image_prefix_len <= 0 || image_prefix_len >= (int)sizeof(image_prefix)) {
        free(response.data);
        free(auth);
        esp_http_client_cleanup(client);
        return build_error_json("image request prefix too large");
    }

    size_t prompt_json_cap = json_escaped_len(prompt) + 3;
    char *prompt_json = doubao_heap_alloc(prompt_json_cap);
    if (prompt_json == NULL) {
        free(response.data);
        free(auth);
        esp_http_client_cleanup(client);
        return build_error_json("not enough memory for image prompt");
    }
    size_t prompt_json_len = 0;
    if (!append_escaped_json_string(prompt_json, prompt_json_cap, &prompt_json_len, prompt)) {
        free(prompt_json);
        free(response.data);
        free(auth);
        esp_http_client_cleanup(client);
        return build_error_json("failed to escape image prompt");
    }

    size_t body_len = (size_t)prefix_len + prompt_json_len + (size_t)image_prefix_len +
                      base64_encoded_len(image_len) + strlen(suffix);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Authorization", auth);

    esp_err_t err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) {
        free(prompt_json);
        free(response.data);
        free(auth);
        esp_http_client_cleanup(client);
        return build_error_json(esp_err_to_name(err));
    }

    err = client_write_all(client, prefix, (size_t)prefix_len);
    if (err == ESP_OK) {
        err = client_write_all(client, prompt_json, prompt_json_len);
    }
    if (err == ESP_OK) {
        err = client_write_all(client, image_prefix, (size_t)image_prefix_len);
    }
    free(prompt_json);
    if (err == ESP_OK) {
        char encoded[DOUBAO_BASE64_CHUNK_ENCODED_BYTES + 4];
        size_t offset = 0;
        while (offset < image_len && err == ESP_OK) {
            size_t chunk_len = image_len - offset;
            if (chunk_len > DOUBAO_BASE64_CHUNK_BYTES) {
                chunk_len = DOUBAO_BASE64_CHUNK_BYTES;
            }

            size_t encoded_len = 0;
            int b64_ret = mbedtls_base64_encode((unsigned char *)encoded,
                                                sizeof(encoded),
                                                &encoded_len,
                                                image + offset,
                                                chunk_len);
            if (b64_ret != 0) {
                err = ESP_FAIL;
                break;
            }
            err = client_write_all(client, encoded, encoded_len);
            offset += chunk_len;
        }
    }
    if (err == ESP_OK) {
        err = client_write_all(client, suffix, strlen(suffix));
    }

    free(auth);
    if (err != ESP_OK) {
        free(response.data);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return build_error_json("failed to stream doubao image request");
    }

    char *result = finish_streamed_doubao_request(client, &response);
    esp_http_client_cleanup(client);
    return result;
}

static char *extract_json_object(const char *text)
{
    if (text == NULL) {
        return NULL;
    }

    char *first_valid = NULL;
    const char *scan = text;
    while ((scan = strchr(scan, '{')) != NULL) {
        bool in_string = false;
        bool escaped = false;
        int depth = 0;
        const char *p = scan;

        while (*p != '\0') {
            char ch = *p;
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (ch == '\\') {
                    escaped = true;
                } else if (ch == '"') {
                    in_string = false;
                }
            } else {
                if (ch == '"') {
                    in_string = true;
                } else if (ch == '{') {
                    depth++;
                } else if (ch == '}') {
                    depth--;
                    if (depth == 0) {
                        char *candidate = dup_range(scan, (size_t)(p - scan + 1));
                        if (candidate == NULL) {
                            free(first_valid);
                            return NULL;
                        }
                        cJSON *root = cJSON_Parse(candidate);
                        if (root != NULL) {
                            bool has_notes = cJSON_GetObjectItemCaseSensitive(root, "notes") != NULL;
                            bool has_choices = cJSON_GetObjectItemCaseSensitive(root, "choices") != NULL;
                            cJSON_Delete(root);
                            if (has_notes || has_choices) {
                                free(first_valid);
                                return candidate;
                            }
                            if (first_valid == NULL) {
                                first_valid = candidate;
                            } else {
                                free(candidate);
                            }
                        } else {
                            free(candidate);
                        }
                        break;
                    }
                }
            }
            p++;
        }
        scan++;
    }

    return first_valid;
}

static const char *find_json_string_value(const char *text, const char *key)
{
    if (text == NULL || key == NULL) {
        return NULL;
    }

    const char *p = text;
    while ((p = strstr(p, key)) != NULL) {
        p += strlen(key);
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (*p != ':') {
            continue;
        }
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

static char *dup_json_string_value(const char *start)
{
    if (start == NULL) {
        return NULL;
    }

    size_t cap = strlen(start) + 1;
    char *out = malloc(cap);
    if (out == NULL) {
        return NULL;
    }

    size_t len = 0;
    const char *p = start;
    while (*p != '\0') {
        unsigned char ch = (unsigned char)*p++;
        if (ch == '"') {
            out[len] = '\0';
            return out;
        }
        if (ch != '\\') {
            out[len++] = (char)ch;
            continue;
        }

        unsigned char esc = (unsigned char)*p++;
        switch (esc) {
        case '"':
        case '\\':
        case '/':
            out[len++] = (char)esc;
            break;
        case 'b':
            out[len++] = '\b';
            break;
        case 'f':
            out[len++] = '\f';
            break;
        case 'n':
            out[len++] = '\n';
            break;
        case 'r':
            out[len++] = '\r';
            break;
        case 't':
            out[len++] = '\t';
            break;
        case 'u':
            /*
             * Keep non-ASCII unicode escapes as spaces. Doubao's useful JSON
             * keys are ASCII, and this avoids adding a larger UTF-16 decoder.
             */
            for (int i = 0; i < 4 && *p != '\0'; ++i) {
                p++;
            }
            out[len++] = ' ';
            break;
        default:
            if (esc != '\0') {
                out[len++] = (char)esc;
            }
            break;
        }
    }

    out[len] = '\0';
    return out;
}

static char *extract_content_json_from_completion(const char *completion_json)
{
    const char *content_start = find_json_string_value(completion_json, "\"content\"");
    char *content = dup_json_string_value(content_start);
    if (content == NULL) {
        return NULL;
    }
    char *result = extract_json_object(content);
    if (result == NULL) {
        log_response_preview("doubao manually extracted content", content);
    }
    free(content);
    return result;
}

static char *extract_json_from_cjson_strings(cJSON *item)
{
    if (item == NULL) {
        return NULL;
    }
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        return extract_json_object(item->valuestring);
    }
    if (cJSON_IsArray(item) || cJSON_IsObject(item)) {
        cJSON *child = item->child;
        while (child != NULL) {
            char *result = extract_json_from_cjson_strings(child);
            if (result != NULL) {
                return result;
            }
            child = child->next;
        }
    }
    return NULL;
}

static char *extract_choice_content_json(const char *completion_json)
{
    const char *parse_start = skip_json_prefix_noise(completion_json);
    cJSON *root = cJSON_Parse(parse_start);
    char *fallback_json = NULL;
    if (root == NULL && parse_start != NULL) {
        fallback_json = extract_json_object(parse_start);
        if (fallback_json != NULL) {
            root = cJSON_Parse(fallback_json);
        }
    }
    if (root == NULL) {
        char *manual_result = extract_content_json_from_completion(completion_json);
        if (manual_result != NULL) {
            free(fallback_json);
            return manual_result;
        }
        const char *err = cJSON_GetErrorPtr();
        ESP_LOGW(TAG,
                 "invalid doubao response json at: %.80s",
                 err != NULL ? err : "<unknown>");
        log_response_preview("doubao raw response", completion_json);
        free(fallback_json);
        return build_error_json("invalid doubao response json; see serial log preview");
    }

    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (cJSON_IsFalse(ok)) {
        cJSON_Delete(root);
        free(fallback_json);
        return dup_string(completion_json);
    }

    cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    cJSON *choice0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = choice0 != NULL ? cJSON_GetObjectItemCaseSensitive(choice0, "message") : NULL;
    cJSON *content = message != NULL ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;
    cJSON *reasoning_content = message != NULL ? cJSON_GetObjectItemCaseSensitive(message, "reasoning_content") : NULL;

    char *result = NULL;
    if (cJSON_IsString(content) && content->valuestring != NULL) {
        result = extract_json_object(content->valuestring);
        if (result == NULL) {
            log_response_preview("doubao message content", content->valuestring);
        }
    }
    if (result == NULL && cJSON_IsString(reasoning_content) && reasoning_content->valuestring != NULL) {
        result = extract_json_object(reasoning_content->valuestring);
        if (result == NULL) {
            log_response_preview("doubao reasoning content", reasoning_content->valuestring);
        }
    }
    if (result == NULL) {
        result = extract_json_from_cjson_strings(root);
    }
    cJSON_Delete(root);
    free(fallback_json);

    if (result == NULL) {
        log_response_preview("doubao parsed response", completion_json);
        return build_error_json("doubao response did not contain a json object");
    }
    return result;
}

char *doubao_recognize_sheet_image(const uint8_t *image,
                                   size_t image_len,
                                   const char *mime_type)
{
    if (image == NULL || image_len == 0) {
        return build_error_json("missing image data");
    }
    if (image_len > DOUBAO_MAX_IMAGE_BYTES) {
        return build_error_json("image too large for AI recognition; compress below 768KB");
    }

    const char *safe_mime = (mime_type != NULL && mime_type[0] != '\0') ? mime_type : "image/jpeg";
    const char *prompt =
        "请尽量准确完整地识别图片中的乐谱主旋律。"
        "最终只返回一个 JSON 对象，不要 markdown，不要把解释文字放在 JSON 外面。"
        "第一个字符必须是 {，最后一个字符必须是 }。"
        "格式：{\"ok\":true,\"title\":\"\",\"bpm\":120,\"time_signature\":\"4/4\","
        "\"notes\":[{\"midi\":60,\"start\":0,\"duration\":0.5}]}。"
        "start 和 duration 用秒。只识别主旋律；如果有歌词、伴奏、和弦或多声部，优先取最高声部/主旋律。"
        "不要因为输出紧凑而省略可见音符，最多 240 个音符，midi 范围 21 到 108。"
        "无法识别时只返回 {\"ok\":false,\"message\":\"unreadable sheet\"}。";

    char *completion = post_image_request_to_doubao(image, image_len, safe_mime, prompt);
    if (completion == NULL) {
        return build_error_json("doubao request failed");
    }
    char *result = extract_choice_content_json(completion);
    free(completion);
    return result;
}

char *doubao_score_performance(const char *context_json)
{
    if (context_json == NULL || context_json[0] == '\0') {
        return build_error_json("missing performance context");
    }

    const char *system_prompt =
        "You are a strict but encouraging music teacher. "
        "Evaluate a student's performance from the supplied local scoring JSON and per-note details. "
        "Return strict JSON only, no markdown. "
        "Schema: {\"ok\":true,\"ai_total_score\":0,\"ai_pitch_score\":0,\"ai_rhythm_score\":0,"
        "\"ai_complete_score\":0,\"level\":\"string\",\"summary\":\"Chinese text\","
        "\"practice_plan\":[\"Chinese text\"],\"focus_notes\":[{\"index\":1,\"issue\":\"Chinese text\"}]}. "
        "Scores are 0 to 100. Use the score metrics and per-note expected/played values as evidence. "
        "Check the supplied local metrics critically and turn the detected weaknesses into concrete practice steps. "
        "Consider pitch, rhythm, duration, missing notes, extra notes, confidence, possible global pitch offset, tempo drift and start offset. "
        "Be useful for training advice, not only grading.";

    const char *prefix = "{\"model\":\"";
    const char *messages_prefix =
        "\",\"temperature\":0.0,\"max_tokens\":1024,\"messages\":["
        "{\"role\":\"system\",\"content\":";
    const char *user_prefix = "},{\"role\":\"user\",\"content\":";
    const char *suffix = "}]}";
    size_t body_cap = strlen(prefix) + strlen(DOUBAO_TEXT_MODEL) + strlen(messages_prefix) +
                      json_escaped_len(system_prompt) + 2 + strlen(user_prefix) +
                      json_escaped_len(context_json) + 2 + strlen(suffix) + 1;
    char *body = doubao_heap_alloc(body_cap);
    if (body == NULL) {
        return build_error_json("not enough memory for score request body");
    }
    size_t body_len = 0;
    bool ok = append_raw_to_buffer(body, body_cap, &body_len, prefix) &&
              append_raw_to_buffer(body, body_cap, &body_len, DOUBAO_TEXT_MODEL) &&
              append_raw_to_buffer(body, body_cap, &body_len, messages_prefix) &&
              append_escaped_json_string(body, body_cap, &body_len, system_prompt) &&
              append_raw_to_buffer(body, body_cap, &body_len, user_prefix) &&
              append_escaped_json_string(body, body_cap, &body_len, context_json) &&
              append_raw_to_buffer(body, body_cap, &body_len, suffix);
    if (!ok) {
        free(body);
        return build_error_json("failed to build score request");
    }
    ESP_LOGI(TAG, "doubao score request bytes=%u context_bytes=%u",
             (unsigned)body_len,
             (unsigned)strlen(context_json));

    char *completion = post_json_to_doubao(body);
    free(body);
    if (completion == NULL) {
        return build_error_json("doubao request failed");
    }
    char *result = extract_choice_content_json(completion);
    free(completion);
    return result;
}
