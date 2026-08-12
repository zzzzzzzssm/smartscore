#include "dashscope_omr_response.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static bool json_u32(const cJSON *item, uint32_t *out_value)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < 0.0 || item->valuedouble > UINT32_MAX) {
        return false;
    }
    double integer_part = 0.0;
    if (modf(item->valuedouble, &integer_part) != 0.0) {
        return false;
    }
    *out_value = (uint32_t)integer_part;
    return true;
}

static char *duplicate_text(const char *text)
{
    size_t length = strlen(text);
    char *copy = malloc(length + 1U);
    if (copy != NULL) {
        memcpy(copy, text, length + 1U);
    }
    return copy;
}

dashscope_omr_error_t dashscope_omr_parse_completion(
    const char *outer_json, size_t length,
    dashscope_omr_parsed_response_t *out_response)
{
    if (out_response != NULL) {
        memset(out_response, 0, sizeof(*out_response));
    }
    if (out_response == NULL) {
        return DASHSCOPE_OMR_ERR_ARGUMENT;
    }
    if (outer_json == NULL || length == 0U) {
        return DASHSCOPE_OMR_ERR_OUTER_JSON;
    }

    char *terminated = malloc(length + 1U);
    if (terminated == NULL) {
        return DASHSCOPE_OMR_ERR_NO_MEMORY;
    }
    memcpy(terminated, outer_json, length);
    terminated[length] = '\0';
    cJSON *root = cJSON_ParseWithLengthOpts(terminated, length + 1U, NULL, true);
    free(terminated);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return DASHSCOPE_OMR_ERR_OUTER_JSON;
    }

    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    const cJSON *choice = cJSON_IsArray(choices)
                              ? cJSON_GetArrayItem(choices, 0)
                              : NULL;
    if (!cJSON_IsObject(choice)) {
        cJSON_Delete(root);
        return DASHSCOPE_OMR_ERR_CHOICES;
    }

    const cJSON *finish_reason =
        cJSON_GetObjectItemCaseSensitive(choice, "finish_reason");
    if (!cJSON_IsString(finish_reason) || finish_reason->valuestring == NULL) {
        cJSON_Delete(root);
        return DASHSCOPE_OMR_ERR_FINISH_REASON;
    }
    if (strcmp(finish_reason->valuestring, "length") == 0) {
        cJSON_Delete(root);
        return DASHSCOPE_OMR_ERR_FINISH_LENGTH;
    }
    if (strcmp(finish_reason->valuestring, "stop") != 0) {
        cJSON_Delete(root);
        return DASHSCOPE_OMR_ERR_FINISH_REASON;
    }

    const cJSON *message = cJSON_GetObjectItemCaseSensitive(choice, "message");
    const cJSON *content = cJSON_IsObject(message)
                               ? cJSON_GetObjectItemCaseSensitive(message,
                                                                  "content")
                               : NULL;
    if (!cJSON_IsString(content) || content->valuestring == NULL ||
        content->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return DASHSCOPE_OMR_ERR_CONTENT;
    }
    out_response->compact_json = duplicate_text(content->valuestring);
    if (out_response->compact_json == NULL) {
        cJSON_Delete(root);
        return DASHSCOPE_OMR_ERR_NO_MEMORY;
    }

    const cJSON *usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
    if (usage != NULL) {
        const cJSON *input = cJSON_IsObject(usage)
                                 ? cJSON_GetObjectItemCaseSensitive(
                                       usage, "prompt_tokens")
                                 : NULL;
        const cJSON *output = cJSON_IsObject(usage)
                                  ? cJSON_GetObjectItemCaseSensitive(
                                        usage, "completion_tokens")
                                  : NULL;
        const cJSON *total = cJSON_IsObject(usage)
                                 ? cJSON_GetObjectItemCaseSensitive(
                                       usage, "total_tokens")
                                 : NULL;
        if (!json_u32(input, &out_response->usage.input_tokens) ||
            !json_u32(output, &out_response->usage.output_tokens) ||
            !json_u32(total, &out_response->usage.total_tokens)) {
            cJSON_Delete(root);
            dashscope_omr_parsed_response_free(out_response);
            return DASHSCOPE_OMR_ERR_USAGE;
        }
        out_response->usage.present = true;
    }
    cJSON_Delete(root);

    compact_score_error_code_t score_result = compact_score_parse(
        out_response->compact_json, strlen(out_response->compact_json),
        &out_response->score, &out_response->score_error);
    if (score_result == COMPACT_SCORE_ERR_JSON) {
        dashscope_omr_parsed_response_free(out_response);
        return DASHSCOPE_OMR_ERR_INNER_JSON;
    }
    if (score_result != COMPACT_SCORE_OK) {
        compact_score_error_t score_error = out_response->score_error;
        dashscope_omr_parsed_response_free(out_response);
        out_response->score_error = score_error;
        return DASHSCOPE_OMR_ERR_SCORE_SCHEMA;
    }
    return DASHSCOPE_OMR_OK;
}

void dashscope_omr_parsed_response_free(
    dashscope_omr_parsed_response_t *response)
{
    if (response == NULL) {
        return;
    }
    free(response->compact_json);
    compact_score_free(response->score);
    memset(response, 0, sizeof(*response));
}

dashscope_omr_error_t dashscope_omr_error_from_http_status(int status)
{
    if (status == 200) return DASHSCOPE_OMR_OK;
    if (status == 401) return DASHSCOPE_OMR_ERR_HTTP_UNAUTHORIZED;
    if (status == 403) return DASHSCOPE_OMR_ERR_HTTP_FORBIDDEN;
    if (status == 429) return DASHSCOPE_OMR_ERR_HTTP_RATE_LIMIT;
    if (status == 500 || status == 502 || status == 503 || status == 504) {
        return DASHSCOPE_OMR_ERR_HTTP_SERVER;
    }
    return DASHSCOPE_OMR_ERR_HTTP_STATUS;
}

bool dashscope_omr_should_retry(dashscope_omr_error_t error, int status)
{
    if (error == DASHSCOPE_OMR_ERR_NETWORK ||
        error == DASHSCOPE_OMR_ERR_TIMEOUT) {
        return true;
    }
    return status == 429 || status == 500 || status == 502 || status == 503 ||
           status == 504;
}

const char *dashscope_omr_error_name(dashscope_omr_error_t error)
{
    switch (error) {
    case DASHSCOPE_OMR_OK: return "ok";
    case DASHSCOPE_OMR_ERR_NOT_CONFIGURED: return "dashscope_key_missing";
    case DASHSCOPE_OMR_ERR_BUSY: return "omr_busy";
    case DASHSCOPE_OMR_ERR_ARGUMENT: return "invalid_argument";
    case DASHSCOPE_OMR_ERR_NO_MEMORY: return "no_memory";
    case DASHSCOPE_OMR_ERR_REQUEST_BUILD: return "request_build_failed";
    case DASHSCOPE_OMR_ERR_NETWORK: return "network_interrupted";
    case DASHSCOPE_OMR_ERR_TIMEOUT: return "dashscope_timeout";
    case DASHSCOPE_OMR_ERR_HTTP_UNAUTHORIZED: return "dashscope_http_401";
    case DASHSCOPE_OMR_ERR_HTTP_FORBIDDEN: return "dashscope_http_403";
    case DASHSCOPE_OMR_ERR_HTTP_RATE_LIMIT: return "dashscope_http_429";
    case DASHSCOPE_OMR_ERR_HTTP_SERVER: return "dashscope_http_5xx";
    case DASHSCOPE_OMR_ERR_HTTP_STATUS: return "dashscope_http_status";
    case DASHSCOPE_OMR_ERR_RESPONSE_TOO_LARGE: return "response_too_large";
    case DASHSCOPE_OMR_ERR_OUTER_JSON: return "invalid_outer_json";
    case DASHSCOPE_OMR_ERR_CHOICES: return "missing_choices";
    case DASHSCOPE_OMR_ERR_FINISH_LENGTH: return "finish_reason_length";
    case DASHSCOPE_OMR_ERR_FINISH_REASON: return "invalid_finish_reason";
    case DASHSCOPE_OMR_ERR_CONTENT: return "missing_message_content";
    case DASHSCOPE_OMR_ERR_USAGE: return "invalid_usage";
    case DASHSCOPE_OMR_ERR_INNER_JSON: return "invalid_inner_json";
    case DASHSCOPE_OMR_ERR_SCORE_SCHEMA: return "compact_score_schema_error";
    case DASHSCOPE_OMR_ERR_DUPLICATE_TASK: return "duplicate_omr_task";
    default: return "unknown_dashscope_omr_error";
    }
}
