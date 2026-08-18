#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_preprocess.h"

typedef enum {
    ADAPTIVE_GAIN_REASON_NONE = 0,
    ADAPTIVE_GAIN_REASON_DEMO_ENTER,
    ADAPTIVE_GAIN_REASON_STRICT_RESTORE,
    ADAPTIVE_GAIN_REASON_CLIPPING,
    ADAPTIVE_GAIN_REASON_LOW_SIGNAL,
} adaptive_gain_reason_t;

typedef struct {
    float current_gain_db;
    unsigned clip_streak;
    uint32_t low_peak_since_ms;
    uint32_t last_change_ms;
} adaptive_input_control_t;

void adaptive_input_control_init(adaptive_input_control_t *control,
                                 float current_gain_db);
bool adaptive_input_control_profile_target(
    const adaptive_input_control_t *control, bool demo_profile,
    float *requested_gain_db, adaptive_gain_reason_t *reason);
bool adaptive_input_control_update(
    adaptive_input_control_t *control, const audio_frame_metrics_t *metrics,
    size_t metric_count, int selected_mic, float selected_snr_db,
    uint32_t now_ms, float *requested_gain_db,
    adaptive_gain_reason_t *reason);
void adaptive_input_control_applied(adaptive_input_control_t *control,
                                    float gain_db, uint32_t now_ms);
const char *adaptive_gain_reason_name(adaptive_gain_reason_t reason);
