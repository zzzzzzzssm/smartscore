#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool initialized;
    bool codec_ready;
    bool output_active;
    bool amp_enabled;
    bool muted;
    uint8_t volume_percent;
    uint32_t sample_rate_hz;
    uint32_t write_errors;
    esp_err_t last_error;
} board_audio_status_t;

esp_err_t board_audio_init(void);
esp_err_t board_audio_deinit(void);
esp_err_t board_audio_force_disabled(void);
esp_err_t board_audio_begin_output(void);
esp_err_t board_audio_end_output(void);
esp_err_t board_audio_write(const int16_t *samples, size_t sample_count);
esp_err_t board_audio_set_volume(uint8_t percent);
esp_err_t board_audio_set_mute(bool muted);
esp_err_t board_audio_set_sample_rate(uint32_t sample_rate_hz);
void board_audio_get_status(board_audio_status_t *out_status);

#ifdef __cplusplus
}
#endif
