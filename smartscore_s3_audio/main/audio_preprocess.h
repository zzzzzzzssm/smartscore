#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    float rms;
    float peak;
    float mean;
    float clip_rate;
    bool clipped;
} audio_frame_metrics_t;

typedef struct {
    float prev_x[2];
    float prev_y[2];
    float noise_rms_sum;
    float noise_peak;
    float noise_floor;
    float noise_gate;
    uint32_t calibration_frames;
    bool calibrated;
} audio_preprocess_state_t;

void audio_preprocess_init(audio_preprocess_state_t *state);
void audio_preprocess_frame(audio_preprocess_state_t *state, const int16_t *input,
                            float *output, size_t count, audio_frame_metrics_t *metrics);
void audio_preprocess_finish_calibration(audio_preprocess_state_t *state);
bool audio_preprocess_above_gate(const audio_preprocess_state_t *state,
                                 const audio_frame_metrics_t *metrics);
