#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "music_detector_config.h"

#define LOW_NOTE_DETECTOR_MIDI_MIN MUSIC_PIANO_MIDI_MIN
#define LOW_NOTE_DETECTOR_MIDI_MAX MUSIC_VIRTUAL_FUNDAMENTAL_MAX_MIDI
#define LOW_NOTE_DETECTOR_KEY_COUNT \
    (LOW_NOTE_DETECTOR_MIDI_MAX - LOW_NOTE_DETECTOR_MIDI_MIN + 1)

typedef struct {
    bool valid;
    uint8_t count;
    int midi[MUSIC_LOW_MATCH_MAX_KEYS];
    float frequency_hz[MUSIC_LOW_MATCH_MAX_KEYS];
    float confidence[MUSIC_LOW_MATCH_MAX_KEYS];
    float key_confidence[LOW_NOTE_DETECTOR_KEY_COUNT];
    int strongest_midi;
    float strongest_confidence;
    float strongest_score;
    bool tonal_override;
} low_note_result_t;

typedef struct {
    float ring[2][MUSIC_LOW_MATCH_RING_SIZE];
    size_t ring_write[2];
    size_t ring_filled[2];
    float fir_history[2][MUSIC_LOW_MATCH_FIR_TAPS];
    size_t fir_write[2];
    uint8_t decimation_phase[2];
    float fir[MUSIC_LOW_MATCH_FIR_TAPS];
    float hann[MUSIC_LOW_MATCH_WINDOW_SIZE];
    float goertzel_coefficient[LOW_NOTE_DETECTOR_KEY_COUNT]
                              [MUSIC_LOW_MATCH_MAX_HARMONICS];
    bool harmonic_valid[LOW_NOTE_DETECTOR_KEY_COUNT]
                       [MUSIC_LOW_MATCH_MAX_HARMONICS];
    float window[MUSIC_LOW_MATCH_WINDOW_SIZE];
    float amplitude[LOW_NOTE_DETECTOR_KEY_COUNT]
                   [MUSIC_LOW_MATCH_MAX_HARMONICS];
    float residual[LOW_NOTE_DETECTOR_KEY_COUNT]
                  [MUSIC_LOW_MATCH_MAX_HARMONICS];
    float noise_floor[MUSIC_LOW_MATCH_MAX_HARMONICS];
    float smoothed_confidence[LOW_NOTE_DETECTOR_KEY_COUNT];
    uint8_t attack_frames[LOW_NOTE_DETECTOR_KEY_COUNT];
    uint8_t release_frames[LOW_NOTE_DETECTOR_KEY_COUNT];
    bool active[LOW_NOTE_DETECTOR_KEY_COUNT];
} low_note_detector_t;

void low_note_detector_init(low_note_detector_t *detector);
void low_note_detector_reset(low_note_detector_t *detector);
void low_note_detector_push(low_note_detector_t *detector, int mic_index,
                            const float *samples, size_t count);
bool low_note_detector_copy_recent(const low_note_detector_t *detector,
                                   int mic_index, float *samples,
                                   size_t count);
void low_note_detector_analyze(low_note_detector_t *detector, int mic_index,
                               bool signal_active, bool allow_attack,
                               bool input_clipped,
                               low_note_result_t *result);
