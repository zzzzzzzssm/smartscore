#pragma once

#include <stdbool.h>

typedef enum {
    MELODY_GATE_REJECTED = 0,
    MELODY_GATE_STRICT,
    MELODY_GATE_SPECTRUM_EXACT,
    MELODY_GATE_HARMONIC_POLY,
    MELODY_GATE_SUPPORTED_YIN,
    MELODY_GATE_STRONG_YIN,
} melody_gate_path_t;

typedef struct {
    float strict_yin_confidence;
    float strict_harmonic_ratio;
    float spectrum_yin_confidence;
    float harmonic_yin_confidence;
    float strong_yin_confidence;
    float supported_yin_confidence;
    int midi_min;
    int midi_max;
} melody_gate_config_t;

typedef struct {
    bool yin_valid;
    float yin_confidence;
    float harmonic_ratio;
    int midi;
    bool signal_above_gate;
    bool clipped;
    bool spectrum_exact_support;
    bool spectrum_pitch_class_support;
    bool polyphony_is_yin_harmonics;
} melody_gate_observation_t;

typedef struct {
    bool accepted;
    bool dominates_polyphony;
    bool confidence_from_yin;
    melody_gate_path_t path;
} melody_gate_decision_t;

melody_gate_decision_t melody_gate_decide(
    const melody_gate_config_t *config,
    const melody_gate_observation_t *observation);
