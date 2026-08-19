#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MUSIC_RESULT_SILENCE = 0,
    MUSIC_RESULT_UNKNOWN,
    MUSIC_RESULT_SINGLE,
    MUSIC_RESULT_INTERVAL,
    MUSIC_RESULT_CHORD
} music_result_type_t;

typedef enum {
    MUSIC_RECOGNITION_PROFILE_STRICT = 0,
    MUSIC_RECOGNITION_PROFILE_DEMO,
} music_recognition_profile_t;

#define MUSIC_DIAGNOSTIC_MAX_RAW_CANDIDATES 4
#define MUSIC_DIAGNOSTIC_MAX_NOTES 3
#define MUSIC_DIAGNOSTIC_REASON_SIZE 32

typedef enum {
    MUSIC_DIAGNOSTIC_SOURCE_YIN = 0,
    MUSIC_DIAGNOSTIC_SOURCE_LOW_YIN,
    MUSIC_DIAGNOSTIC_SOURCE_LOW_TEMPLATE,
    MUSIC_DIAGNOSTIC_SOURCE_SPECTRUM,
} music_diagnostic_source_t;

typedef struct {
    int midi;
    float frequency_hz;
    float confidence;
    music_diagnostic_source_t source;
} music_diagnostic_candidate_t;

typedef struct {
    uint32_t timestamp_ms;
    int raw_candidate_count;
    music_diagnostic_candidate_t
        raw_candidates[MUSIC_DIAGNOSTIC_MAX_RAW_CANDIDATES];
    music_result_type_t candidate_type;
    int candidate_note_count;
    int candidate_notes[MUSIC_DIAGNOSTIC_MAX_NOTES];
    music_result_type_t final_type;
    int final_note_count;
    int final_notes[MUSIC_DIAGNOSTIC_MAX_NOTES];
    int octave_shift;
    float band_snr_db[3];
    char reject_reason[MUSIC_DIAGNOSTIC_REASON_SIZE];
} music_diagnostic_t;

typedef struct {
    music_result_type_t type;
    float rms;
    float mic1_rms;
    float mic2_rms;
    float confidence;
    int selected_mic;
    float frequency_hz;
    int midi;
    float cents;
    char note_name[8];
    float yin_confidence;
    float harmonic_explained_ratio;
    bool onset;
    bool octave_corrected;
    int chord_root;
    bool chord_is_minor;
    char chord_name[16];
    int pitch_classes[4];
    int midi_notes[4];
    int pitch_class_count;
    float chroma[12];
    uint32_t timestamp_ms;
} music_result_t;

int music_detector_start(void);
void music_detector_set_recognition_profile(music_recognition_profile_t profile);
music_recognition_profile_t music_detector_get_recognition_profile(void);
