#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_inmp441_init(void);
esp_err_t audio_inmp441_prepare_frame(size_t frame_samples);
esp_err_t audio_inmp441_read_pcm(int16_t *pcm, size_t sample_count);
void audio_inmp441_deinit(void);

#ifdef __cplusplus
}
#endif
