#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dashscope_omr_response.h"
#include "score_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DASHSCOPE_OMR_MODEL "qwen3.8-max"
#define DASHSCOPE_OMR_ENDPOINT \
    "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"

typedef struct {
    dashscope_omr_error_t error;
    int http_status;
    uint32_t elapsed_ms;
    uint8_t attempt_count;
    dashscope_omr_usage_t usage;
    char *compact_json;
    compact_score_document_t *score;
    compact_score_error_t score_error;
} dashscope_omr_result_t;

bool dashscope_omr_api_key_configured(void);

dashscope_omr_error_t dashscope_omr_recognize(
    const score_capture_t *capture,
    dashscope_omr_result_t *out_result);

void dashscope_omr_result_free(dashscope_omr_result_t *result);

extern const char dashscope_omr_prompt[];

#ifdef __cplusplus
}
#endif

