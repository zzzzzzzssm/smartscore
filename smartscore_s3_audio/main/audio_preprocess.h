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
    float hp_x1;
    float hp_x2;
    float hp_y1;
    float hp_y2;
    float noise_rms_sum;
    float noise_peak;
    float noise_floor;
    float noise_gate;
    uint32_t calibration_frames;
    uint32_t calibration_rejected_frames;
    bool calibrated;
} audio_preprocess_state_t;

void audio_preprocess_init(audio_preprocess_state_t *state);
void audio_preprocess_frame(audio_preprocess_state_t *state, const int16_t *input,
                            float *output, size_t count, audio_frame_metrics_t *metrics);
bool audio_preprocess_calibration_ready(const audio_preprocess_state_t *state);
void audio_preprocess_finish_calibration(audio_preprocess_state_t *state);
bool audio_preprocess_above_gate(const audio_preprocess_state_t *state,
                                 const audio_frame_metrics_t *metrics);
void audio_preprocess_rescale_gain(audio_preprocess_state_t *state,
                                   float linear_scale);
void audio_preprocess_track_ambient(audio_preprocess_state_t *state,
                                    const audio_frame_metrics_t *metrics);
