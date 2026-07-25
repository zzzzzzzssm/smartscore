#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_audio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPEAKER_FILE_NAME_MAX 256

typedef enum {
    SPEAKER_STATE_UNINITIALIZED = 0,
    SPEAKER_STATE_STOPPED,
    SPEAKER_STATE_TONE,
    SPEAKER_STATE_METRONOME,
    SPEAKER_STATE_FILE,
    SPEAKER_STATE_FILE_PAUSED,
    SPEAKER_STATE_ERROR,
} speaker_state_t;

typedef enum {
    SPEAKER_METRONOME_STOPPED = 0,
    SPEAKER_METRONOME_RUNNING,
    SPEAKER_METRONOME_PAUSED,
} speaker_metronome_state_t;

typedef struct {
    speaker_metronome_state_t state;
    uint16_t bpm;
    uint8_t beats_per_measure;
    uint8_t beat_unit;
    uint8_t beat_index;
    uint32_t queue_errors;
    uint32_t task_stack_min_words;
} speaker_metronome_status_t;

typedef struct {
    speaker_state_t state;
    bool hardware_output_enabled;
    float frequency_hz;
    uint32_t duration_ms;
    uint32_t elapsed_ms;
    char file_name[SPEAKER_FILE_NAME_MAX];
    bool sd_present;
    uint64_t sd_capacity_bytes;
    esp_err_t sd_last_error;
    esp_err_t last_error;
    uint32_t task_stack_min_words;
    speaker_metronome_status_t metronome;
    board_audio_status_t hardware;
} speaker_status_t;

esp_err_t speaker_service_init(void);
/* Initialize the same control/status API without ES8311, I2S or PCM output. */
esp_err_t speaker_service_init_control_only(void);
bool speaker_service_is_ready(void);
esp_err_t speaker_service_play_tone(float frequency_hz,
                                    uint32_t duration_ms,
                                    float gain);
esp_err_t speaker_service_stop(void);
esp_err_t speaker_service_set_volume(uint8_t percent);
esp_err_t speaker_service_set_mute(bool muted);

esp_err_t speaker_service_metronome_start(uint16_t bpm,
                                          uint8_t beats_per_measure,
                                          uint8_t beat_unit);
esp_err_t speaker_service_metronome_pause(void);
esp_err_t speaker_service_metronome_stop(void);

esp_err_t speaker_service_list_files(
    char names[][SPEAKER_FILE_NAME_MAX],
    size_t capacity,
    size_t *out_count);
esp_err_t speaker_service_list_files_page(
    const char *search,
    size_t requested_page,
    size_t page_size,
    char names[][SPEAKER_FILE_NAME_MAX],
    size_t capacity,
    size_t *out_count,
    size_t *out_total,
    size_t *out_page);
esp_err_t speaker_service_play_file(const char *safe_name);
esp_err_t speaker_service_pause_file(void);
esp_err_t speaker_service_stop_file(void);

void speaker_service_get_status(speaker_status_t *out_status);

#ifdef __cplusplus
}
#endif
