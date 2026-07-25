#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t score_json_parse_and_store(const char *json, size_t len, char *error, size_t error_len);
esp_err_t score_json_parse_and_store_with_tempo(const char *json, size_t len,
                                                 int tempo_bpm,
                                                 char *error,
                                                 size_t error_len);

#ifdef __cplusplus
}
#endif
