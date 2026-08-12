#include "deepseek_advice.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#ifndef DEEPSEEK_API_KEY
#define DEEPSEEK_API_KEY "__DEEPSEEK_API_KEY__"
#endif

#define DEEPSEEK_ADVICE_URL "https://api.deepseek.com/chat/completions"
#define DEEPSEEK_ADVICE_MODEL "deepseek-v4-flash"
#define DEEPSEEK_HTTP_TIMEOUT_MS 120000
#define DEEPSEEK_HTTP_BUFFER_BYTES 4096
#define DEEPSEEK_RESPONSE_INITIAL_BYTES (8U * 1024U)
#define DEEPSEEK_RESPONSE_MAX_BYTES (64U * 1024U)
#define DEEPSEEK_MAX_ISSUES 40U

static const char *TAG = "DEEPSEEK_ADVICE";

static const char system_prompt[] =
    "你是音乐练习指导助手。用户会提供由设备客观计算的练习JSON数据。"
    "不得修改、质疑或重新计算本地评分；只能依据输入中的分数、统计和错误事件给出具体、可执行的练习建议。"
    "禁止杜撰输入中不存在的错误位置或演奏问题。必须返回严格JSON。";

static const char user_prompt_prefix[] =
    "请分析下面由本地设备产生的音乐练习数据。\n\n"
    "规则：\n"
    "1. scores和overall均为本地算法最终评分，不得修改、重新计算或输出不同分数。\n"
    "2. 只能引用JSON中存在的音符索引、错误类型和统计数据。\n"
    "3. 不得根据曲名或音乐常识杜撰演奏问题。\n"
    "4. 找出最应该优先改进的1至3个问题。\n"
    "5. 每条建议必须包括数据依据、具体练习动作、建议速度、练习时长、重复次数和达标条件。\n"
    "6. 建议必须适合下一次真实练习，禁止只输出空泛内容。\n"
    "7. 如果输入数据不足以判断某个问题，写入insufficient_data，不要猜测。\n"
    "8. 只返回严格合法的JSON，不要Markdown或额外解释。\n"
    "9. 不得在输出中包含任何评分字段。\n"
    "10. evaluable为false的维度只能写入insufficient_data，不得作为问题依据。\n"
    "11. evidence优先引用错误次数和音符索引，不要复述分数值。\n"
    "12. focus的rank和steps的order必须从1连续递增，total_minutes必须等于各步骤minutes之和。\n\n"
    "输出JSON格式：\n"
    "{\"v\":1,\"summary\":\"一句话总结\",\"focus\":[{\"rank\":1,"
    "\"dimension_id\":\"rhythm\",\"problem\":\"具体问题\","
    "\"evidence\":[\"数据依据\"],\"practice\":{\"action\":\"练习方法\","
    "\"bpm\":60,\"minutes\":5,\"repetitions\":4,\"target\":\"达标条件\"}}],"
    "\"next_session\":{\"total_minutes\":15,\"steps\":[{\"order\":1,"
    "\"action\":\"练习内容\",\"minutes\":5}]},\"encouragement\":\"具体鼓励\","
    "\"insufficient_data\":[]}\n\n练习数据JSON：\n";

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool overflow;
} response_buffer_t;

typedef struct {
    const cJSON *detail;
    int priority;
    double magnitude;
    size_t original_index;
} issue_candidate_t;

static bool finite_number(const cJSON *item)
{
    return cJSON_IsNumber(item) && isfinite(item->valuedouble);
}

static bool integer_in_range(const cJSON *item, int minimum, int maximum)
{
    if (!finite_number(item) || item->valuedouble < minimum ||
        item->valuedouble > maximum) {
        return false;
    }
    double integer_part = 0.0;
    return modf(item->valuedouble, &integer_part) == 0.0;
}

static bool nonempty_string(const cJSON *item)
{
    return cJSON_IsString(item) && item->valuestring != NULL &&
           item->valuestring[0] != '\0';
}

static bool response_reserve(response_buffer_t *buffer, size_t required)
{
    if (required <= buffer->capacity) {
        return true;
    }
    if (required > DEEPSEEK_RESPONSE_MAX_BYTES) {
        buffer->overflow = true;
        return false;
    }
    size_t capacity = buffer->capacity == 0U
                          ? DEEPSEEK_RESPONSE_INITIAL_BYTES
                          : buffer->capacity;
    while (capacity < required && capacity < DEEPSEEK_RESPONSE_MAX_BYTES) {
        capacity *= 2U;
    }
    if (capacity > DEEPSEEK_RESPONSE_MAX_BYTES) {
        capacity = DEEPSEEK_RESPONSE_MAX_BYTES;
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

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    response_buffer_t *buffer = (response_buffer_t *)event->user_data;
    return response_append(buffer, (const char *)event->data,
                           (size_t)event->data_len)
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

static bool add_number_if_present(cJSON *output, const char *output_name,
                                  const cJSON *input, const char *input_name)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(input, input_name);
    if (!finite_number(value)) {
        return true;
    }
    return cJSON_AddNumberToObject(output, output_name, value->valuedouble) !=
           NULL;
}

static bool add_score(cJSON *scores, const cJSON *source, const char *id,
                      const char *name, const char *field, bool evaluable)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(source, field);
    if (!finite_number(value)) {
        return false;
    }
    cJSON *entry = cJSON_CreateObject();
    if (entry == NULL ||
        cJSON_AddStringToObject(entry, "id", id) == NULL ||
        cJSON_AddStringToObject(entry, "name", name) == NULL ||
        cJSON_AddNumberToObject(entry, "value", value->valuedouble) == NULL ||
        cJSON_AddNumberToObject(entry, "max", 100) == NULL ||
        cJSON_AddBoolToObject(entry, "evaluable", evaluable) == NULL ||
        !cJSON_AddItemToArray(scores, entry)) {
        cJSON_Delete(entry);
        return false;
    }
    return true;
}

static int issue_priority(const char *result)
{
    if (strcmp(result, "pitch_and_rhythm_error") == 0) return 0;
    if (strcmp(result, "missing") == 0) return 1;
    if (strcmp(result, "pitch_error") == 0) return 2;
    if (strcmp(result, "rhythm_error") == 0) return 3;
    if (strcmp(result, "extra") == 0 || strcmp(result, "retry") == 0) return 4;
    if (strcmp(result, "uncertain") == 0) return 5;
    return -1;
}

static double signed_offset_ms(const cJSON *detail, bool *available)
{
    const cJSON *expected = cJSON_GetObjectItemCaseSensitive(
        detail, "expected_start_ms");
    const cJSON *aligned = cJSON_GetObjectItemCaseSensitive(
        detail, "aligned_played_start_ms");
    if (!finite_number(expected) || !finite_number(aligned) ||
        expected->valuedouble < 0.0 || aligned->valuedouble < 0.0) {
        *available = false;
        return 0.0;
    }
    *available = true;
    return aligned->valuedouble - expected->valuedouble;
}

static int compare_candidates(const void *left, const void *right)
{
    const issue_candidate_t *a = (const issue_candidate_t *)left;
    const issue_candidate_t *b = (const issue_candidate_t *)right;
    if (a->priority != b->priority) {
        return a->priority < b->priority ? -1 : 1;
    }
    if (a->magnitude != b->magnitude) {
        return a->magnitude > b->magnitude ? -1 : 1;
    }
    if (a->original_index == b->original_index) return 0;
    return a->original_index < b->original_index ? -1 : 1;
}

static const char *normalized_issue_type(const char *result, double offset_ms,
                                         bool offset_available)
{
    if (strcmp(result, "missing") == 0) return "missed_note";
    if (strcmp(result, "extra") == 0) return "extra_note";
    if (strcmp(result, "retry") == 0) return "retry";
    if (strcmp(result, "uncertain") == 0) return "uncertain";
    if (strcmp(result, "pitch_error") == 0) return "wrong_pitch";
    if (strcmp(result, "rhythm_error") == 0) {
        if (!offset_available) return "rhythm_error";
        return offset_ms < 0.0 ? "early" : "late";
    }
    if (strcmp(result, "pitch_and_rhythm_error") == 0) {
        if (!offset_available) return "wrong_pitch_and_rhythm";
        return offset_ms < 0.0 ? "wrong_pitch_and_early"
                               : "wrong_pitch_and_late";
    }
    return result;
}

static bool add_issue_number(cJSON *issue, const char *output_name,
                             const cJSON *detail, const char *input_name,
                             bool allow_negative)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(detail, input_name);
    if (!finite_number(value) || (!allow_negative && value->valuedouble < 0.0)) {
        return true;
    }
    return cJSON_AddNumberToObject(issue, output_name, value->valuedouble) !=
           NULL;
}

static deepseek_advice_error_t build_advice_context(
    const char *score_json, char **out_context)
{
    *out_context = NULL;
    cJSON *source = cJSON_ParseWithOpts(score_json, NULL, true);
    if (!cJSON_IsObject(source)) {
        cJSON_Delete(source);
        return DEEPSEEK_ADVICE_ERR_INPUT_JSON;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *piece = root != NULL ? cJSON_AddObjectToObject(root, "piece") : NULL;
    cJSON *scores = root != NULL ? cJSON_AddArrayToObject(root, "scores") : NULL;
    cJSON *overall = root != NULL ? cJSON_AddObjectToObject(root, "overall") : NULL;
    cJSON *stats = root != NULL ? cJSON_AddObjectToObject(root, "stats") : NULL;
    cJSON *issues = root != NULL ? cJSON_AddArrayToObject(root, "issues") : NULL;
    if (root == NULL || piece == NULL || scores == NULL || overall == NULL ||
        stats == NULL || issues == NULL ||
        cJSON_AddNumberToObject(root, "v", 1) == NULL) {
        cJSON_Delete(source);
        cJSON_Delete(root);
        return DEEPSEEK_ADVICE_ERR_NO_MEMORY;
    }

    const cJSON *title = cJSON_GetObjectItemCaseSensitive(source, "title");
    if (nonempty_string(title) &&
        cJSON_AddStringToObject(piece, "name", title->valuestring) == NULL) {
        cJSON_Delete(source);
        cJSON_Delete(root);
        return DEEPSEEK_ADVICE_ERR_NO_MEMORY;
    }
    if (!add_number_if_present(piece, "practice_bpm", source, "practice_bpm") ||
        !add_number_if_present(piece, "duration_sec", source,
                               "piece_duration_sec")) {
        cJSON_Delete(source);
        cJSON_Delete(root);
        return DEEPSEEK_ADVICE_ERR_NO_MEMORY;
    }

    const cJSON *rhythm_evaluable = cJSON_GetObjectItemCaseSensitive(
        source, "rhythm_evaluable");
    if (!add_score(scores, source, "pitch", "音准", "pitch_score", true) ||
        !add_score(scores, source, "rhythm", "节奏", "rhythm_score",
                   !cJSON_IsFalse(rhythm_evaluable)) ||
        !add_score(scores, source, "continuity", "连贯性", "fluency_score",
                   true)) {
        cJSON_Delete(source);
        cJSON_Delete(root);
        return DEEPSEEK_ADVICE_ERR_INPUT_JSON;
    }
    const cJSON *total = cJSON_GetObjectItemCaseSensitive(source, "total_score");
    if (!finite_number(total) ||
        cJSON_AddNumberToObject(overall, "value", total->valuedouble) == NULL ||
        cJSON_AddNumberToObject(overall, "max", 100) == NULL) {
        cJSON_Delete(source);
        cJSON_Delete(root);
        return DEEPSEEK_ADVICE_ERR_INPUT_JSON;
    }

    if (!add_number_if_present(stats, "total_notes", source, "target_count") ||
        !add_number_if_present(stats, "attempted_notes", source,
                               "attempted_count") ||
        !add_number_if_present(stats, "wrong_pitch", source, "wrong_count") ||
        !add_number_if_present(stats, "missed_notes", source, "missing_count") ||
        !add_number_if_present(stats, "extra_notes", source, "extra_count") ||
        !add_number_if_present(stats, "retry_notes", source, "retry_count") ||
        !add_number_if_present(stats, "uncertain_notes", source,
                               "uncertain_count")) {
        cJSON_Delete(source);
        cJSON_Delete(root);
        return DEEPSEEK_ADVICE_ERR_NO_MEMORY;
    }

    const cJSON *details = cJSON_GetObjectItemCaseSensitive(source, "details");
    size_t detail_count = cJSON_IsArray(details)
                              ? (size_t)cJSON_GetArraySize(details)
                              : 0U;
    issue_candidate_t *candidates = detail_count > 0U
                                        ? calloc(detail_count,
                                                 sizeof(*candidates))
                                        : NULL;
    if (detail_count > 0U && candidates == NULL) {
        cJSON_Delete(source);
        cJSON_Delete(root);
        return DEEPSEEK_ADVICE_ERR_NO_MEMORY;
    }

    size_t candidate_count = 0U;
    unsigned early_count = 0U;
    unsigned late_count = 0U;
    for (size_t index = 0; index < detail_count; ++index) {
        const cJSON *detail = cJSON_GetArrayItem(details, (int)index);
        const cJSON *result_item = cJSON_IsObject(detail)
                                       ? cJSON_GetObjectItemCaseSensitive(
                                             detail, "result")
                                       : NULL;
        if (!nonempty_string(result_item)) continue;
        int priority = issue_priority(result_item->valuestring);
        if (priority < 0) continue;

        bool offset_available = false;
        double offset = signed_offset_ms(detail, &offset_available);
        if ((strcmp(result_item->valuestring, "rhythm_error") == 0 ||
             strcmp(result_item->valuestring,
                    "pitch_and_rhythm_error") == 0) &&
            offset_available) {
            if (offset < 0.0) ++early_count;
            else if (offset > 0.0) ++late_count;
        }
        const cJSON *pitch = cJSON_GetObjectItemCaseSensitive(
            detail, "pitch_error_semitones");
        double magnitude = offset_available ? fabs(offset) : 0.0;
        if (finite_number(pitch)) {
            magnitude += fabs(pitch->valuedouble) * 100000.0;
        }
        candidates[candidate_count++] = (issue_candidate_t){
            .detail = detail,
            .priority = priority,
            .magnitude = magnitude,
            .original_index = index,
        };
    }
    if (candidate_count > 1U) {
        qsort(candidates, candidate_count, sizeof(*candidates),
              compare_candidates);
    }

    if (cJSON_AddNumberToObject(stats, "early_notes", early_count) == NULL ||
        cJSON_AddNumberToObject(stats, "late_notes", late_count) == NULL) {
        free(candidates);
        cJSON_Delete(source);
        cJSON_Delete(root);
        return DEEPSEEK_ADVICE_ERR_NO_MEMORY;
    }

    size_t selected = candidate_count < DEEPSEEK_MAX_ISSUES
                          ? candidate_count
                          : DEEPSEEK_MAX_ISSUES;
    for (size_t index = 0; index < selected; ++index) {
        const cJSON *detail = candidates[index].detail;
        const cJSON *result_item = cJSON_GetObjectItemCaseSensitive(
            detail, "result");
        bool offset_available = false;
        double offset = signed_offset_ms(detail, &offset_available);
        cJSON *issue = cJSON_CreateObject();
        if (issue == NULL ||
            cJSON_AddStringToObject(
                issue, "type",
                normalized_issue_type(result_item->valuestring, offset,
                                      offset_available)) == NULL ||
            !add_issue_number(issue, "ref_index", detail, "ref_index", false) ||
            !add_issue_number(issue, "expected_midi", detail,
                              "expected_midi", false) ||
            !add_issue_number(issue, "actual_midi", detail,
                              "played_midi", false)) {
            cJSON_Delete(issue);
            free(candidates);
            cJSON_Delete(source);
            cJSON_Delete(root);
            return DEEPSEEK_ADVICE_ERR_NO_MEMORY;
        }
        if (offset_available &&
            cJSON_AddNumberToObject(issue, "offset_ms", offset) == NULL) {
            cJSON_Delete(issue);
            free(candidates);
            cJSON_Delete(source);
            cJSON_Delete(root);
            return DEEPSEEK_ADVICE_ERR_NO_MEMORY;
        }
        if (!cJSON_AddItemToArray(issues, issue)) {
            cJSON_Delete(issue);
            free(candidates);
            cJSON_Delete(source);
            cJSON_Delete(root);
            return DEEPSEEK_ADVICE_ERR_NO_MEMORY;
        }
    }
    free(candidates);
    cJSON_Delete(source);

    char *context = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (context == NULL) {
        return DEEPSEEK_ADVICE_ERR_NO_MEMORY;
    }
    *out_context = context;
    return DEEPSEEK_ADVICE_OK;
}

static deepseek_advice_error_t build_request(const char *context,
                                             char **out_request)
{
    *out_request = NULL;
    size_t prompt_length = strlen(user_prompt_prefix) + strlen(context) + 1U;
    char *user_prompt = malloc(prompt_length);
    if (user_prompt == NULL) return DEEPSEEK_ADVICE_ERR_NO_MEMORY;
    snprintf(user_prompt, prompt_length, "%s%s", user_prompt_prefix, context);

    cJSON *root = cJSON_CreateObject();
    cJSON *messages = root != NULL
                          ? cJSON_AddArrayToObject(root, "messages")
                          : NULL;
    cJSON *system = cJSON_CreateObject();
    cJSON *user = cJSON_CreateObject();
    cJSON *thinking = root != NULL
                          ? cJSON_AddObjectToObject(root, "thinking")
                          : NULL;
    cJSON *format = root != NULL
                        ? cJSON_AddObjectToObject(root, "response_format")
                        : NULL;
    bool system_attached = false;
    bool user_attached = false;
    bool ok = root != NULL && messages != NULL && system != NULL && user != NULL &&
              thinking != NULL && format != NULL;
    if (ok) {
        ok = cJSON_AddStringToObject(root, "model", DEEPSEEK_ADVICE_MODEL) != NULL &&
             cJSON_AddStringToObject(system, "role", "system") != NULL &&
             cJSON_AddStringToObject(system, "content", system_prompt) != NULL &&
             cJSON_AddStringToObject(user, "role", "user") != NULL &&
             cJSON_AddStringToObject(user, "content", user_prompt) != NULL;
    }
    if (ok) {
        system_attached = cJSON_AddItemToArray(messages, system);
        ok = system_attached;
    }
    if (ok) {
        user_attached = cJSON_AddItemToArray(messages, user);
        ok = user_attached;
    }
    if (ok) {
        ok = cJSON_AddStringToObject(thinking, "type", "disabled") != NULL &&
             cJSON_AddStringToObject(format, "type", "json_object") != NULL &&
             cJSON_AddNumberToObject(root, "temperature", 0.2) != NULL &&
             cJSON_AddNumberToObject(root, "max_tokens", 1500) != NULL &&
             cJSON_AddBoolToObject(root, "stream", false) != NULL;
    }
    free(user_prompt);
    if (!ok) {
        if (system != NULL && !system_attached) cJSON_Delete(system);
        if (user != NULL && !user_attached) cJSON_Delete(user);
        cJSON_Delete(root);
        return DEEPSEEK_ADVICE_ERR_NO_MEMORY;
    }

    char *request = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (request == NULL) return DEEPSEEK_ADVICE_ERR_NO_MEMORY;
    *out_request = request;
    return DEEPSEEK_ADVICE_OK;
}

static deepseek_advice_error_t post_request(const char *body,
                                            response_buffer_t *response,
                                            int *out_status)
{
    esp_http_client_config_t config = {
        .url = DEEPSEEK_ADVICE_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = DEEPSEEK_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = DEEPSEEK_HTTP_BUFFER_BYTES,
        .buffer_size_tx = DEEPSEEK_HTTP_BUFFER_BYTES,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return DEEPSEEK_ADVICE_ERR_NO_MEMORY;

    size_t authorization_length = strlen("Bearer ") +
                                  strlen(DEEPSEEK_API_KEY) + 1U;
    char *authorization = malloc(authorization_length);
    if (authorization == NULL) {
        esp_http_client_cleanup(client);
        return DEEPSEEK_ADVICE_ERR_NO_MEMORY;
    }
    snprintf(authorization, authorization_length, "Bearer %s",
             DEEPSEEK_API_KEY);
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t transport = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    memset(authorization, 0, authorization_length);
    free(authorization);
    esp_http_client_cleanup(client);

    if (response->overflow) return DEEPSEEK_ADVICE_ERR_RESPONSE_TOO_LARGE;
    if (transport == ESP_ERR_NO_MEM) return DEEPSEEK_ADVICE_ERR_NO_MEMORY;
    if (transport == ESP_ERR_TIMEOUT || transport == ESP_ERR_HTTP_EAGAIN) {
        return DEEPSEEK_ADVICE_ERR_TIMEOUT;
    }
    if (transport != ESP_OK) return DEEPSEEK_ADVICE_ERR_NETWORK;
    if (*out_status < 200 || *out_status >= 300) {
        return DEEPSEEK_ADVICE_ERR_HTTP_STATUS;
    }
    if (response->data == NULL || response->length == 0U) {
        return DEEPSEEK_ADVICE_ERR_CONTENT;
    }
    return DEEPSEEK_ADVICE_OK;
}

static bool key_contains_score(const char *key)
{
    if (key == NULL) return false;
    const char needle[] = "score";
    for (const char *cursor = key; *cursor != '\0'; ++cursor) {
        size_t index = 0U;
        while (needle[index] != '\0' && cursor[index] != '\0' &&
               (char)tolower((unsigned char)cursor[index]) == needle[index]) {
            ++index;
        }
        if (needle[index] == '\0') return true;
    }
    return strcmp(key, "overall") == 0;
}

static bool contains_score_field(const cJSON *item)
{
    if (item == NULL) return false;
    for (const cJSON *child = item->child; child != NULL; child = child->next) {
        if (key_contains_score(child->string) || contains_score_field(child)) {
            return true;
        }
    }
    return false;
}

static bool validate_string_array(const cJSON *array, int minimum,
                                  int maximum)
{
    if (!cJSON_IsArray(array)) return false;
    int count = cJSON_GetArraySize(array);
    if (count < minimum || count > maximum) return false;
    for (int index = 0; index < count; ++index) {
        if (!nonempty_string(cJSON_GetArrayItem(array, index))) return false;
    }
    return true;
}

static bool validate_advice(const cJSON *root)
{
    if (!cJSON_IsObject(root) || contains_score_field(root) ||
        !integer_in_range(cJSON_GetObjectItemCaseSensitive(root, "v"), 1, 1) ||
        !nonempty_string(cJSON_GetObjectItemCaseSensitive(root, "summary")) ||
        !nonempty_string(cJSON_GetObjectItemCaseSensitive(root,
                                                          "encouragement")) ||
        !validate_string_array(cJSON_GetObjectItemCaseSensitive(
                                   root, "insufficient_data"),
                               0, 10)) {
        return false;
    }

    const cJSON *focus = cJSON_GetObjectItemCaseSensitive(root, "focus");
    int focus_count = cJSON_IsArray(focus) ? cJSON_GetArraySize(focus) : 0;
    if (focus_count < 1 || focus_count > 3) return false;
    for (int index = 0; index < focus_count; ++index) {
        const cJSON *item = cJSON_GetArrayItem(focus, index);
        const cJSON *dimension = cJSON_IsObject(item)
                                     ? cJSON_GetObjectItemCaseSensitive(
                                           item, "dimension_id")
                                     : NULL;
        const cJSON *practice = cJSON_IsObject(item)
                                    ? cJSON_GetObjectItemCaseSensitive(
                                          item, "practice")
                                    : NULL;
        bool valid_dimension = nonempty_string(dimension) &&
            (strcmp(dimension->valuestring, "pitch") == 0 ||
             strcmp(dimension->valuestring, "rhythm") == 0 ||
             strcmp(dimension->valuestring, "continuity") == 0);
        if (!cJSON_IsObject(item) || !valid_dimension ||
            !integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "rank"),
                              index + 1, index + 1) ||
            !nonempty_string(cJSON_GetObjectItemCaseSensitive(item,
                                                               "problem")) ||
            !validate_string_array(cJSON_GetObjectItemCaseSensitive(
                                       item, "evidence"),
                                   1, 5) ||
            !cJSON_IsObject(practice) ||
            !nonempty_string(cJSON_GetObjectItemCaseSensitive(practice,
                                                               "action")) ||
            !integer_in_range(cJSON_GetObjectItemCaseSensitive(practice, "bpm"),
                              20, 400) ||
            !integer_in_range(cJSON_GetObjectItemCaseSensitive(practice,
                                                                "minutes"),
                              1, 60) ||
            !integer_in_range(cJSON_GetObjectItemCaseSensitive(
                                  practice, "repetitions"),
                              1, 100) ||
            !nonempty_string(cJSON_GetObjectItemCaseSensitive(practice,
                                                               "target"))) {
            return false;
        }
    }

    const cJSON *session = cJSON_GetObjectItemCaseSensitive(root,
                                                             "next_session");
    const cJSON *steps = cJSON_IsObject(session)
                             ? cJSON_GetObjectItemCaseSensitive(session,
                                                                "steps")
                             : NULL;
    int step_count = cJSON_IsArray(steps) ? cJSON_GetArraySize(steps) : 0;
    if (!cJSON_IsObject(session) || step_count < 1 || step_count > 10 ||
        !integer_in_range(cJSON_GetObjectItemCaseSensitive(
                              session, "total_minutes"),
                          1, 120)) {
        return false;
    }
    int minute_sum = 0;
    for (int index = 0; index < step_count; ++index) {
        const cJSON *step = cJSON_GetArrayItem(steps, index);
        const cJSON *minutes = cJSON_IsObject(step)
                                   ? cJSON_GetObjectItemCaseSensitive(
                                         step, "minutes")
                                   : NULL;
        if (!cJSON_IsObject(step) ||
            !integer_in_range(cJSON_GetObjectItemCaseSensitive(step, "order"),
                              index + 1, index + 1) ||
            !nonempty_string(cJSON_GetObjectItemCaseSensitive(step,
                                                               "action")) ||
            !integer_in_range(minutes, 1, 60)) {
            return false;
        }
        minute_sum += (int)minutes->valuedouble;
    }
    return minute_sum == (int)cJSON_GetObjectItemCaseSensitive(
                                  session, "total_minutes")->valuedouble;
}

static deepseek_advice_error_t parse_completion(
    const response_buffer_t *response, char **out_advice)
{
    cJSON *outer = cJSON_ParseWithLengthOpts(response->data,
                                             response->length + 1U,
                                             NULL, true);
    if (!cJSON_IsObject(outer)) {
        cJSON_Delete(outer);
        return DEEPSEEK_ADVICE_ERR_OUTER_JSON;
    }
    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(outer, "choices");
    const cJSON *choice = cJSON_IsArray(choices)
                              ? cJSON_GetArrayItem(choices, 0)
                              : NULL;
    const cJSON *finish = cJSON_IsObject(choice)
                              ? cJSON_GetObjectItemCaseSensitive(
                                    choice, "finish_reason")
                              : NULL;
    if (!nonempty_string(finish) || strcmp(finish->valuestring, "stop") != 0) {
        cJSON_Delete(outer);
        return DEEPSEEK_ADVICE_ERR_FINISH_REASON;
    }
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(choice, "message");
    const cJSON *content = cJSON_IsObject(message)
                               ? cJSON_GetObjectItemCaseSensitive(message,
                                                                  "content")
                               : NULL;
    if (!nonempty_string(content)) {
        cJSON_Delete(outer);
        return DEEPSEEK_ADVICE_ERR_CONTENT;
    }
    cJSON *advice = cJSON_ParseWithOpts(content->valuestring, NULL, true);
    cJSON_Delete(outer);
    if (!cJSON_IsObject(advice)) {
        cJSON_Delete(advice);
        return DEEPSEEK_ADVICE_ERR_INNER_JSON;
    }
    if (!validate_advice(advice)) {
        cJSON_Delete(advice);
        return DEEPSEEK_ADVICE_ERR_SCHEMA;
    }
    char *json = cJSON_PrintUnformatted(advice);
    cJSON_Delete(advice);
    if (json == NULL) return DEEPSEEK_ADVICE_ERR_NO_MEMORY;
    *out_advice = json;
    return DEEPSEEK_ADVICE_OK;
}

bool deepseek_advice_api_key_configured(void)
{
    return DEEPSEEK_API_KEY[0] != '\0' &&
           strcmp(DEEPSEEK_API_KEY, "__DEEPSEEK_API_KEY__") != 0;
}

deepseek_advice_error_t deepseek_advice_generate(
    const char *local_score_json, char **out_advice_json)
{
    if (out_advice_json != NULL) *out_advice_json = NULL;
    if (local_score_json == NULL || local_score_json[0] == '\0' ||
        out_advice_json == NULL) {
        return DEEPSEEK_ADVICE_ERR_ARGUMENT;
    }
    if (!deepseek_advice_api_key_configured()) {
        return DEEPSEEK_ADVICE_ERR_NOT_CONFIGURED;
    }

    char *context = NULL;
    deepseek_advice_error_t error = build_advice_context(local_score_json,
                                                         &context);
    if (error != DEEPSEEK_ADVICE_OK) return error;
    char *request = NULL;
    error = build_request(context, &request);
    cJSON_free(context);
    if (error != DEEPSEEK_ADVICE_OK) return error;

    response_buffer_t response = {0};
    int http_status = 0;
    error = post_request(request, &response, &http_status);
    cJSON_free(request);
    if (error == DEEPSEEK_ADVICE_OK) {
        error = parse_completion(&response, out_advice_json);
    }
    ESP_LOGI(TAG, "advice request status=%d response_bytes=%u result=%s",
             http_status, (unsigned)response.length,
             deepseek_advice_error_name(error));
    free(response.data);
    return error;
}

const char *deepseek_advice_error_name(deepseek_advice_error_t error)
{
    switch (error) {
    case DEEPSEEK_ADVICE_OK: return "ok";
    case DEEPSEEK_ADVICE_ERR_NOT_CONFIGURED: return "deepseek_key_missing";
    case DEEPSEEK_ADVICE_ERR_ARGUMENT: return "invalid_argument";
    case DEEPSEEK_ADVICE_ERR_NO_MEMORY: return "no_memory";
    case DEEPSEEK_ADVICE_ERR_INPUT_JSON: return "invalid_local_score_json";
    case DEEPSEEK_ADVICE_ERR_REQUEST_BUILD: return "request_build_failed";
    case DEEPSEEK_ADVICE_ERR_NETWORK: return "network_interrupted";
    case DEEPSEEK_ADVICE_ERR_TIMEOUT: return "deepseek_timeout";
    case DEEPSEEK_ADVICE_ERR_HTTP_STATUS: return "deepseek_http_status";
    case DEEPSEEK_ADVICE_ERR_RESPONSE_TOO_LARGE: return "response_too_large";
    case DEEPSEEK_ADVICE_ERR_OUTER_JSON: return "invalid_outer_json";
    case DEEPSEEK_ADVICE_ERR_FINISH_REASON: return "invalid_finish_reason";
    case DEEPSEEK_ADVICE_ERR_CONTENT: return "missing_message_content";
    case DEEPSEEK_ADVICE_ERR_INNER_JSON: return "invalid_advice_json";
    case DEEPSEEK_ADVICE_ERR_SCHEMA: return "invalid_advice_schema";
    default: return "unknown_deepseek_advice_error";
    }
}
