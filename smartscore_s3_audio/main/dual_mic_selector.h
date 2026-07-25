#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audio_preprocess.h"

typedef enum {
    DUAL_MIC_CHANNEL_INVALID = 0,
    DUAL_MIC_CHANNEL_WEAK,
    DUAL_MIC_CHANNEL_VALID,
    DUAL_MIC_CHANNEL_GOOD,
    DUAL_MIC_CHANNEL_CLIPPED,
    DUAL_MIC_CHANNEL_NOISY,
} dual_mic_channel_state_t;

typedef enum {
    DUAL_MIC_HEALTH_BOTH_INVALID = 0,
    DUAL_MIC_HEALTH_MIC1_ONLY,
    DUAL_MIC_HEALTH_MIC2_ONLY,
    DUAL_MIC_HEALTH_DUAL_OK,
} dual_mic_health_t;

typedef struct {
    dual_mic_channel_state_t state;
    float score;
    float snr_db;
    bool signal_valid;
    uint16_t valid_streak;
    uint16_t invalid_streak;
} dual_mic_quality_t;

typedef struct {
    int selected_mic;
    unsigned challenger_frames;
    unsigned hold_frames;
    unsigned inactive_frames;
    bool note_locked;
    uint32_t switch_count;
    dual_mic_quality_t quality[2];
    dual_mic_health_t health;
} dual_mic_selector_t;

typedef struct {
    int selected_mic;
    bool sound_active;
    bool switched;
    dual_mic_health_t health;
} dual_mic_selection_t;

void dual_mic_selector_init(dual_mic_selector_t *selector);
void dual_mic_selector_update(dual_mic_selector_t *selector,
                              const audio_frame_metrics_t metrics[2],
                              const audio_preprocess_state_t preprocess[2],
                              dual_mic_selection_t *selection);
const char *dual_mic_channel_state_name(dual_mic_channel_state_t state);
const char *dual_mic_health_name(dual_mic_health_t health);
