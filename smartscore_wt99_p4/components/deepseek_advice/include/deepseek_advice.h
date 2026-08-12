#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEEPSEEK_ADVICE_OK = 0,
    DEEPSEEK_ADVICE_ERR_NOT_CONFIGURED,
    DEEPSEEK_ADVICE_ERR_ARGUMENT,
    DEEPSEEK_ADVICE_ERR_NO_MEMORY,
    DEEPSEEK_ADVICE_ERR_INPUT_JSON,
    DEEPSEEK_ADVICE_ERR_REQUEST_BUILD,
    DEEPSEEK_ADVICE_ERR_NETWORK,
    DEEPSEEK_ADVICE_ERR_TIMEOUT,
    DEEPSEEK_ADVICE_ERR_HTTP_STATUS,
    DEEPSEEK_ADVICE_ERR_RESPONSE_TOO_LARGE,
    DEEPSEEK_ADVICE_ERR_OUTER_JSON,
    DEEPSEEK_ADVICE_ERR_FINISH_REASON,
    DEEPSEEK_ADVICE_ERR_CONTENT,
    DEEPSEEK_ADVICE_ERR_INNER_JSON,
    DEEPSEEK_ADVICE_ERR_SCHEMA,
} deepseek_advice_error_t;

bool deepseek_advice_api_key_configured(void);

/*
 * Builds a bounded advice context from the last local scoring result and asks
 * DeepSeek for advice. On success, out_advice_json owns a malloc-allocated,
 * validated JSON object that the caller must free.
 */
deepseek_advice_error_t deepseek_advice_generate(
    const char *local_score_json, char **out_advice_json);

const char *deepseek_advice_error_name(deepseek_advice_error_t error);

#ifdef __cplusplus
}
#endif

