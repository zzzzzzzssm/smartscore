#include "melody_gate.h"

static melody_gate_decision_t accepted(melody_gate_path_t path,
                                       bool dominates_polyphony,
                                       bool confidence_from_yin)
{
    return (melody_gate_decision_t) {
        .accepted = true,
        .dominates_polyphony = dominates_polyphony,
        .confidence_from_yin = confidence_from_yin,
        .path = path,
    };
}

melody_gate_decision_t melody_gate_decide(
    const melody_gate_config_t *config,
    const melody_gate_observation_t *observation)
{
    if (config == 0 || observation == 0 || !observation->yin_valid) {
        return (melody_gate_decision_t) {0};
    }

    if (observation->yin_confidence >= config->strict_yin_confidence &&
        observation->harmonic_ratio >= config->strict_harmonic_ratio) {
        return accepted(MELODY_GATE_STRICT, false, false);
    }

    const bool override_input_usable =
        observation->signal_above_gate && !observation->clipped &&
        observation->midi >= config->midi_min &&
        observation->midi <= config->midi_max;
    if (!override_input_usable) {
        return (melody_gate_decision_t) {0};
    }

    if (observation->yin_confidence >= config->spectrum_yin_confidence &&
        observation->spectrum_exact_support) {
        return accepted(MELODY_GATE_SPECTRUM_EXACT, true, true);
    }
    if (observation->yin_confidence >= config->harmonic_yin_confidence &&
        observation->polyphony_is_yin_harmonics) {
        return accepted(MELODY_GATE_HARMONIC_POLY, true, true);
    }
    if (observation->yin_confidence >= config->strong_yin_confidence) {
        return accepted(MELODY_GATE_STRONG_YIN, true, true);
    }
    if (observation->yin_confidence >= config->supported_yin_confidence &&
        observation->spectrum_pitch_class_support) {
        return accepted(MELODY_GATE_SUPPORTED_YIN, true, true);
    }

    return (melody_gate_decision_t) {0};
}
