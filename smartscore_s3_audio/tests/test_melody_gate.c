#include <stdbool.h>
#include <stdio.h>

#include "melody_gate.h"

static int s_failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n",                   \
                    __FILE__, __LINE__, #condition);                         \
            ++s_failures;                                                    \
        }                                                                    \
    } while (0)

static melody_gate_config_t default_config(void)
{
    return (melody_gate_config_t) {
        .strict_yin_confidence = 0.70f,
        .strict_harmonic_ratio = 0.74f,
        .spectrum_yin_confidence = 0.88f,
        .harmonic_yin_confidence = 0.80f,
        .strong_yin_confidence = 0.90f,
        .supported_yin_confidence = 0.80f,
        .midi_min = 48,
        .midi_max = 84,
    };
}

static melody_gate_observation_t playable_note(void)
{
    return (melody_gate_observation_t) {
        .yin_valid = true,
        .yin_confidence = 0.95f,
        .harmonic_ratio = 0.03f,
        .midi = 69,
        .signal_above_gate = true,
    };
}

static void test_strong_yin_overrides_low_harmonic_ratio(void)
{
    const melody_gate_config_t config = default_config();
    const melody_gate_decision_t decision =
        melody_gate_decide(&config, &(melody_gate_observation_t) {
            .yin_valid = true,
            .yin_confidence = 0.95f,
            .harmonic_ratio = 0.02f,
            .midi = 83,
            .signal_above_gate = true,
        });
    CHECK(decision.accepted);
    CHECK(decision.dominates_polyphony);
    CHECK(decision.confidence_from_yin);
    CHECK(decision.path == MELODY_GATE_STRONG_YIN);
}

static void test_strict_path_remains_available(void)
{
    const melody_gate_config_t config = default_config();
    melody_gate_observation_t observation = playable_note();
    observation.yin_confidence = 0.76f;
    observation.harmonic_ratio = 0.82f;
    observation.midi = 87;
    const melody_gate_decision_t decision =
        melody_gate_decide(&config, &observation);
    CHECK(decision.accepted);
    CHECK(decision.path == MELODY_GATE_STRICT);
    CHECK(!decision.confidence_from_yin);
}

static void test_supported_fallback_requires_spectrum_agreement(void)
{
    const melody_gate_config_t config = default_config();
    melody_gate_observation_t observation = playable_note();
    observation.yin_confidence = 0.84f;
    observation.spectrum_pitch_class_support = true;
    melody_gate_decision_t decision =
        melody_gate_decide(&config, &observation);
    CHECK(decision.accepted);
    CHECK(decision.path == MELODY_GATE_SUPPORTED_YIN);

    observation.spectrum_pitch_class_support = false;
    decision = melody_gate_decide(&config, &observation);
    CHECK(!decision.accepted);
}

static void test_safety_guards_reject_unusable_override_input(void)
{
    const melody_gate_config_t config = default_config();
    melody_gate_observation_t observation = playable_note();
    observation.signal_above_gate = false;
    CHECK(!melody_gate_decide(&config, &observation).accepted);

    observation = playable_note();
    observation.clipped = true;
    CHECK(!melody_gate_decide(&config, &observation).accepted);

    observation = playable_note();
    observation.midi = 85;
    CHECK(!melody_gate_decide(&config, &observation).accepted);

    observation = playable_note();
    observation.yin_valid = false;
    CHECK(!melody_gate_decide(&config, &observation).accepted);
}

static void test_existing_spectral_and_harmonic_paths_remain_supported(void)
{
    const melody_gate_config_t config = default_config();
    melody_gate_observation_t observation = playable_note();
    observation.yin_confidence = 0.89f;
    observation.spectrum_exact_support = true;
    melody_gate_decision_t decision =
        melody_gate_decide(&config, &observation);
    CHECK(decision.accepted);
    CHECK(decision.path == MELODY_GATE_SPECTRUM_EXACT);

    observation = playable_note();
    observation.yin_confidence = 0.82f;
    observation.polyphony_is_yin_harmonics = true;
    decision = melody_gate_decide(&config, &observation);
    CHECK(decision.accepted);
    CHECK(decision.path == MELODY_GATE_HARMONIC_POLY);
}

int main(void)
{
    test_strong_yin_overrides_low_harmonic_ratio();
    test_strict_path_remains_available();
    test_supported_fallback_requires_spectrum_agreement();
    test_safety_guards_reject_unusable_override_input();
    test_existing_spectral_and_harmonic_paths_remain_supported();

    if (s_failures != 0) {
        fprintf(stderr, "%d melody gate test(s) failed\n", s_failures);
        return 1;
    }
    puts("melody gate tests passed");
    return 0;
}
