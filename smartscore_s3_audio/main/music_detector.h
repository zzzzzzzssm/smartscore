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
    MUSIC_RECOGNITION_PROFILE_PERFORMANCE,
} music_recognition_profile_t;

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
