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
