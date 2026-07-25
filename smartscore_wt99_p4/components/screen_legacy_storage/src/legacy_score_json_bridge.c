#include <stddef.h>

#include "esp_err.h"
#include "screen_score_bridge.h"

esp_err_t legacy_score_json_parse_and_store(const char *json,
                                            size_t length,
                                            char *error,
                                            size_t error_length)
{
    return screen_score_bridge_parse_and_store(json, length, 0, error,
                                               error_length, NULL);
}

esp_err_t legacy_score_json_parse_and_store_with_tempo(
    const char *json,
    size_t length,
    int tempo_bpm,
    char *error,
    size_t error_length)
{
    return screen_score_bridge_parse_and_store(json, length, tempo_bpm,
                                               error, error_length, NULL);
}
