#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t screen_score_bridge_parse_and_store(const char *json,
                                               size_t length,
                                               int practice_bpm,
                                               char *error,
                                               size_t error_length,
                                               size_t *out_note_count);

#ifdef __cplusplus
}
#endif
