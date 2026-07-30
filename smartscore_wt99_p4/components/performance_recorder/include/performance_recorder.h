#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "performance_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PERFORMANCE_RECORDER_MAX_ACTIVE_NOTES 64
#define PERFORMANCE_RECORDER_MAX_NOTES 1024

typedef enum {
    PERFORMANCE_RECORDER_UNINITIALIZED = 0,
    PERFORMANCE_RECORDER_IDLE,
    PERFORMANCE_RECORDER_RECORDING,
    PERFORMANCE_RECORDER_PAUSED,
    PERFORMANCE_RECORDER_STOPPED,
    PERFORMANCE_RECORDER_ERROR,
} performance_recorder_state_t;

typedef struct {
    performance_recorder_state_t state;
    input_source_t input_source;
    size_t note_count;
    size_t note_capacity;
    size_t active_count;
    size_t observed_note_count;
    size_t uncertain_note_count;
    float confidence_sum;
    bool buffer_in_psram;
    esp_err_t last_error;
    char error[64];
    char message[96];
} performance_recorder_status_t;

esp_err_t performance_recorder_init(void);
esp_err_t performance_recorder_start(input_source_t source);
esp_err_t performance_recorder_pause(void);
esp_err_t performance_recorder_resume(void);
void performance_recorder_process_midi(bool note_on,
                                       uint8_t midi,
                                       uint8_t velocity,
                                       uint8_t channel,
                                       uint64_t timestamp_us);
void performance_recorder_process_audio(bool note_on,
                                        uint8_t midi,
                                        uint8_t velocity,
                                        uint64_t timestamp_us,
                                        float confidence,
                                        float frequency_hz);
esp_err_t performance_recorder_stop_and_take_snapshot(
    performance_snapshot_t *out_snapshot);
void performance_recorder_abort(esp_err_t error,
                                const char *code,
                                const char *message);
void performance_recorder_get_status(performance_recorder_status_t *out_status);
void performance_snapshot_release(performance_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
