#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_preprocess.h"
#include "music_detector_config.h"
#include "yin_detector.h"

typedef enum {
    LOW_FREQUENCY_REJECT_NONE = 0,
    LOW_FREQUENCY_REJECT_NOT_READY,
    LOW_FREQUENCY_REJECT_BELOW_BAND_GATE,
    LOW_FREQUENCY_REJECT_LOW_BAND_SNR,
    LOW_FREQUENCY_REJECT_YIN_LOW_CONFIDENCE,
    LOW_FREQUENCY_REJECT_HARMONIC_MISMATCH,
    LOW_FREQUENCY_REJECT_OCTAVE_UNCONFIRMED,
    LOW_FREQUENCY_REJECT_POLY_SINGLE_DOMINANT,
    LOW_FREQUENCY_REJECT_POLY_NO_INDEPENDENT_SUPPORT,
    LOW_FREQUENCY_REJECT_STABILIZING,
    LOW_FREQUENCY_REJECT_CLIPPING,
    LOW_FREQUENCY_REJECT_SILENCE,
} low_frequency_reject_reason_t;

typedef enum {
    LOW_FREQUENCY_RESULT_NONE = 0,
    LOW_FREQUENCY_RESULT_SINGLE,
    LOW_FREQUENCY_RESULT_INTERVAL,
    LOW_FREQUENCY_RESULT_CHORD,
} low_frequency_result_kind_t;

typedef struct {
    int midi;
    float frequency_hz;
    float score;
    float relative_score;
    float prominence;
    float unique_support;
} low_frequency_candidate_t;

typedef struct {
    bool ready;
    bool spectrum_ready;
    bool triggered;
    yin_result_t yin;
    float harmonic_ratio;
    bool octave_corrected;
    int octave_shift;
    low_frequency_result_kind_t candidate_kind;
    low_frequency_result_kind_t final_kind;
    int midi_notes[3];
    int note_count;
    float confidence;
    int chord_root;
    bool chord_is_minor;
    char chord_name[16];
    int debug_candidate_count;
    low_frequency_candidate_t
        debug_candidates[MUSIC_LOW_DEBUG_CANDIDATE_COUNT];
    float band_snr_db[AUDIO_PREPROCESS_BAND_COUNT];
    low_frequency_reject_reason_t reject_reason;
    uint32_t yin_time_us;
    uint32_t spectrum_time_us;
} low_frequency_result_t;

void low_frequency_analyzer_init(void);
void low_frequency_analyzer_reset(void);
void low_frequency_analyzer_push(const float *samples, size_t count,
                                 int selected_mic);
void low_frequency_analyzer_analyze(bool triggered, bool run_spectrum,
                                    bool low_band_above_gate,
                                    bool clipped, bool onset,
                                    float low_band_noise_floor,
                                    const float band_snr_db[
                                        AUDIO_PREPROCESS_BAND_COUNT],
                                    low_frequency_result_t *result);
const char *low_frequency_reject_reason_name(
    low_frequency_reject_reason_t reason);
