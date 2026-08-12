#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t s3_voice_adpcm_decode(const uint8_t *input,
                                size_t input_length,
                                int16_t *pcm,
                                size_t pcm_capacity,
                                size_t *sample_count);

#ifdef __cplusplus
}
#endif
