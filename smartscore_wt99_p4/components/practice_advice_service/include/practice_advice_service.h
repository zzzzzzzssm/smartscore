#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PRACTICE_ADVICE_SESSION_ID_CAPACITY 64
#define PRACTICE_ADVICE_ERROR_CAPACITY 64
#define PRACTICE_ADVICE_MESSAGE_CAPACITY 160

typedef enum {
    PRACTICE_ADVICE_NONE = 0,
    PRACTICE_ADVICE_WAITING_SCORE,
    PRACTICE_ADVICE_RUNNING,
    PRACTICE_ADVICE_READY,
    PRACTICE_ADVICE_SKIPPED_OFFLINE,
    PRACTICE_ADVICE_FAILED,
} practice_advice_state_t;

typedef struct {
    bool initialized;
    practice_advice_state_t state;
    uint32_t generation;
    char session_id[PRACTICE_ADVICE_SESSION_ID_CAPACITY];
    char error[PRACTICE_ADVICE_ERROR_CAPACITY];
    char message[PRACTICE_ADVICE_MESSAGE_CAPACITY];
    size_t advice_length;
} practice_advice_status_t;

esp_err_t practice_advice_service_init(void);
esp_err_t practice_advice_service_begin_session(char *out_session_id,
                                                size_t capacity);
esp_err_t practice_advice_service_submit_score(const char *score_json,
                                               size_t length);
void practice_advice_service_score_failed(const char *error,
                                          const char *message);
void practice_advice_service_get_status(practice_advice_status_t *status);
esp_err_t practice_advice_service_copy_advice(char **out_json,
                                              size_t *out_length);
const char *practice_advice_state_name(practice_advice_state_t state);

#ifdef __cplusplus
}
#endif
