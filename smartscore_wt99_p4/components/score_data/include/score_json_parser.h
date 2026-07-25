#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t score_json_parse_and_store(const char *json,
                                     size_t length,
                                     char *error,
                                     size_t error_length,
                                     size_t *out_note_count);

#ifdef __cplusplus
}
#endif
