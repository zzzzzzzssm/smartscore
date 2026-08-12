#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Independent IMA-ADPCM blocks keep UART retransmission self-contained. */
esp_err_t voice_ima_adpcm_encode(const int16_t *pcm,
                                 size_t sample_count,
                                 uint8_t *output,
                                 size_t output_capacity,
                                 size_t *output_length);

#ifdef __cplusplus
}
#endif
