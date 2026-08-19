#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_PREPROCESS_BAND_COUNT 3

typedef enum {
    AUDIO_PREPROCESS_BAND_LOW = 0,
    AUDIO_PREPROCESS_BAND_MID,
    AUDIO_PREPROCESS_BAND_HIGH,
} audio_preprocess_band_t;

typedef struct {
    float rms;
    float band_rms[AUDIO_PREPROCESS_BAND_COUNT];
    float peak;
    float mean;
    float clip_rate;
    bool clipped;
} audio_frame_metrics_t;

typedef struct {
    float prev_x[2];
    float prev_y[2];
    float band_lowpass[2];
    float noise_rms_sum;
    float band_noise_rms_sum[AUDIO_PREPROCESS_BAND_COUNT];
    float noise_peak;
    float noise_floor;
    float noise_gate;
    float band_noise_floor[AUDIO_PREPROCESS_BAND_COUNT];
    float band_noise_gate[AUDIO_PREPROCESS_BAND_COUNT];
    uint32_t calibration_frames;
    bool calibrated;
} audio_preprocess_state_t;

void audio_preprocess_init(audio_preprocess_state_t *state);
void audio_preprocess_frame(audio_preprocess_state_t *state, const int16_t *input,
                            float *output, size_t count, audio_frame_metrics_t *metrics);
void audio_preprocess_finish_calibration(audio_preprocess_state_t *state);
bool audio_preprocess_above_gate(const audio_preprocess_state_t *state,
                                 const audio_frame_metrics_t *metrics);
bool audio_preprocess_band_above_gate(const audio_preprocess_state_t *state,
                                      const audio_frame_metrics_t *metrics,
                                      audio_preprocess_band_t band);
bool audio_preprocess_any_band_above_gate(const audio_preprocess_state_t *state,
                                          const audio_frame_metrics_t *metrics);
float audio_preprocess_band_snr_db(const audio_preprocess_state_t *state,
                                   const audio_frame_metrics_t *metrics,
                                   audio_preprocess_band_t band);
void audio_preprocess_rescale_gain(audio_preprocess_state_t *state,
                                   float linear_scale);
void audio_preprocess_track_ambient(audio_preprocess_state_t *state,
                                    const audio_frame_metrics_t *metrics);
