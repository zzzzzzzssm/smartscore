#pragma once

#include <stdbool.h>
#include "audio_preprocess.h"
#include "chord_detector.h"
#include "music_detector.h"
#include "music_detector_config.h"
#include "yin_detector.h"

typedef struct {
    music_result_type_t type_history[MUSIC_STABLE_HISTORY_SIZE];
    int identity_history[MUSIC_STABLE_HISTORY_SIZE];
    bool minor_history[MUSIC_STABLE_HISTORY_SIZE];
    bool onset_history[MUSIC_STABLE_HISTORY_SIZE];
    int history_count;
    int history_position;
    music_result_t last_emitted;
    bool has_last_emitted;
    uint32_t last_emit_ms;
    uint32_t last_onset_ms;
    uint32_t pending_attack_ms;
    float previous_rms;
} music_classifier_t;

void music_classifier_init(music_classifier_t *classifier);
bool music_classifier_update(music_classifier_t *classifier,
                             const audio_frame_metrics_t *mic1_metrics,
                             const audio_frame_metrics_t *mic2_metrics,
                             float mic1_gate, float mic2_gate, int selected_mic,
                             const yin_result_t *yin, float harmonic_ratio,
                             const chord_result_t *chord, uint32_t timestamp_ms,
                             music_result_t *result, const char **unknown_reason);
