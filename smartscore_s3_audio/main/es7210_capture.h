#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "music_detector_config.h"

typedef struct {
    int16_t mic1[MUSIC_CAPTURE_FRAMES];
#if !MUSIC_USE_SINGLE_MIC_CH1
    int16_t mic2[MUSIC_CAPTURE_FRAMES];
#endif
    uint32_t timestamp_ms;
} audio_capture_block_t;

esp_err_t es7210_capture_init(void);
esp_err_t es7210_capture_start(void);
esp_err_t es7210_capture_set_input_gain(float gain_db);
float es7210_capture_get_input_gain(void);
int es7210_capture_take_block(audio_capture_block_t **block, uint32_t timeout_ms);
void es7210_capture_release_block(int index);
unsigned es7210_capture_queue_depth(void);
