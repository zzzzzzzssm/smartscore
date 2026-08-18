#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    CHORD_DETECTION_NONE = 0,
    CHORD_DETECTION_INTERVAL,
    CHORD_DETECTION_MAJOR,
    CHORD_DETECTION_MINOR,
} chord_detection_kind_t;

#define CHORD_DEBUG_CANDIDATE_COUNT 4

typedef struct {
    int midi;
    int peak_bin;
    float peak_frequency_hz;
    float relative_score;
    float prominence;
    bool distinct_local_peak;
} chord_candidate_debug_t;

typedef struct {
    bool valid;
    chord_detection_kind_t kind;
    int identity;
    int root;
    bool is_minor;
    char name[16];
    int pitch_classes[4];
    int midi_notes[4];
    int pitch_class_count;
    int independent_pitch_class_count;
    float confidence;
    float chroma[12];
    float spectrum_noise_floor;
    float band_noise_floor[3];
    int harmonic_rejected_count;
    int debug_candidate_count;
    chord_candidate_debug_t debug_candidates[CHORD_DEBUG_CANDIDATE_COUNT];
} chord_result_t;

int chord_detector_init(void);
void chord_detector_analyze(const float *mic1_ring, const float *mic2_ring,
                            size_t write_position, float mic1_weight,
                            float mic2_weight, bool demo_profile,
                            float signal_snr_db, chord_result_t *result);
float chord_detector_harmonic_explained_ratio(float fundamental_hz);
