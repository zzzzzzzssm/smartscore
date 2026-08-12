#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compact_score.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DASHSCOPE_OMR_OK = 0,
    DASHSCOPE_OMR_ERR_NOT_CONFIGURED,
    DASHSCOPE_OMR_ERR_BUSY,
    DASHSCOPE_OMR_ERR_ARGUMENT,
    DASHSCOPE_OMR_ERR_NO_MEMORY,
    DASHSCOPE_OMR_ERR_REQUEST_BUILD,
    DASHSCOPE_OMR_ERR_NETWORK,
    DASHSCOPE_OMR_ERR_TIMEOUT,
    DASHSCOPE_OMR_ERR_HTTP_UNAUTHORIZED,
    DASHSCOPE_OMR_ERR_HTTP_FORBIDDEN,
    DASHSCOPE_OMR_ERR_HTTP_RATE_LIMIT,
    DASHSCOPE_OMR_ERR_HTTP_SERVER,
    DASHSCOPE_OMR_ERR_HTTP_STATUS,
    DASHSCOPE_OMR_ERR_RESPONSE_TOO_LARGE,
    DASHSCOPE_OMR_ERR_OUTER_JSON,
    DASHSCOPE_OMR_ERR_CHOICES,
    DASHSCOPE_OMR_ERR_FINISH_LENGTH,
    DASHSCOPE_OMR_ERR_FINISH_REASON,
    DASHSCOPE_OMR_ERR_CONTENT,
    DASHSCOPE_OMR_ERR_USAGE,
    DASHSCOPE_OMR_ERR_INNER_JSON,
    DASHSCOPE_OMR_ERR_SCORE_SCHEMA,
    DASHSCOPE_OMR_ERR_DUPLICATE_TASK,
} dashscope_omr_error_t;

typedef struct {
    bool present;
    uint32_t input_tokens;
    uint32_t output_tokens;
    uint32_t total_tokens;
} dashscope_omr_usage_t;

typedef struct {
    char *compact_json;
    compact_score_document_t *score;
    dashscope_omr_usage_t usage;
    compact_score_error_t score_error;
} dashscope_omr_parsed_response_t;

dashscope_omr_error_t dashscope_omr_parse_completion(
    const char *outer_json,
    size_t length,
    dashscope_omr_parsed_response_t *out_response);

void dashscope_omr_parsed_response_free(
    dashscope_omr_parsed_response_t *response);

dashscope_omr_error_t dashscope_omr_error_from_http_status(int status);

bool dashscope_omr_should_retry(dashscope_omr_error_t error, int status);

const char *dashscope_omr_error_name(dashscope_omr_error_t error);

#ifdef __cplusplus
}
#endif
