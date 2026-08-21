#include "music_classifier.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "melody_gate.h"
#include "music_detector_config.h"
#include "note_utils.h"

void music_classifier_init(music_classifier_t *classifier)
{
    memset(classifier, 0, sizeof(*classifier));
}

static int vote_count(const music_classifier_t *classifier, music_result_type_t type,
                      int identity, bool minor)
{
    int count = 0;
    for (int i = 0; i < classifier->history_count; ++i) {
        if (classifier->type_history[i] == type && classifier->identity_history[i] == identity &&
            (type != MUSIC_RESULT_CHORD || classifier->minor_history[i] == minor)) {
            ++count;
        }
    }
    return count;
}

static bool candidate_identity_matches(music_result_type_t left_type,
                                       int left_identity, bool left_minor,
                                       music_result_type_t right_type,
                                       int right_identity, bool right_minor)
{
    return left_type == right_type && left_identity == right_identity &&
           (left_type != MUSIC_RESULT_CHORD || left_minor == right_minor);
}

static void update_consecutive_candidate(music_classifier_t *classifier,
                                         music_result_type_t type,
                                         int identity, bool minor)
{
    if (type == MUSIC_RESULT_UNKNOWN || type == MUSIC_RESULT_SILENCE) {
        classifier->consecutive_type = type;
        classifier->consecutive_identity = identity;
        classifier->consecutive_minor = minor;
        classifier->consecutive_count = 0;
        return;
    }
    if (candidate_identity_matches(classifier->consecutive_type,
                                   classifier->consecutive_identity,
                                   classifier->consecutive_minor,
                                   type, identity, minor)) {
        if (classifier->consecutive_count < MUSIC_STABLE_HISTORY_SIZE) {
            ++classifier->consecutive_count;
        }
        return;
    }
    classifier->consecutive_type = type;
    classifier->consecutive_identity = identity;
    classifier->consecutive_minor = minor;
    classifier->consecutive_count = 1;
}

static bool changed(const music_result_t *left, const music_result_t *right)
{
    if (left->type != right->type) return true;
    if (left->type == MUSIC_RESULT_SINGLE) return left->midi != right->midi;
    if (left->type == MUSIC_RESULT_INTERVAL) {
        return left->pitch_classes[0] != right->pitch_classes[0] ||
               left->pitch_classes[1] != right->pitch_classes[1];
    }
    if (left->type == MUSIC_RESULT_CHORD) {
        return left->chord_root != right->chord_root || left->chord_is_minor != right->chord_is_minor;
    }
    return false;
}

static int result_identity(const music_result_t *result)
{
    if (result->type == MUSIC_RESULT_SINGLE) return result->midi;
    if (result->type == MUSIC_RESULT_INTERVAL) {
        return result->pitch_classes[0] * 12 + result->pitch_classes[1];
    }
    if (result->type == MUSIC_RESULT_CHORD) {
        return result->chord_root * 2 + (result->chord_is_minor ? 1 : 0);
    }
    return -1;
}

static bool frequency_matches_yin_harmonic(float frequency_hz,
                                           float fundamental_hz)
{
    if (frequency_hz <= 0.0f || fundamental_hz <= 0.0f) return false;
    const float ratio = frequency_hz / fundamental_hz;
    if (fabsf(ratio - 1.0f) <= MUSIC_MELODY_HARMONIC_REL_TOLERANCE) {
        return true;
    }
    const float harmonic = roundf(ratio);
    if (harmonic < 2.0f || harmonic > 5.0f) return false;
    return fabsf(ratio - harmonic) <=
           harmonic * MUSIC_MELODY_HARMONIC_REL_TOLERANCE;
}

static bool poly_result_is_yin_harmonics(const yin_result_t *yin,
                                         const chord_result_t *chord)
{
    if (!yin->valid || !chord->valid ||
        yin->confidence < MUSIC_MELODY_YIN_OVERRIDE_CONFIDENCE ||
        chord->pitch_class_count < 2) {
        return false;
    }

    int checked_notes = 0;
    for (int index = 0; index < chord->pitch_class_count; ++index) {
        const int midi = chord->midi_notes[index];
        if (midi < 0 || midi > 127) continue;
        const float frequency_hz =
            MUSIC_REFERENCE_A4_HZ * powf(2.0f, (float)(midi - 69) / 12.0f);
        if (!frequency_matches_yin_harmonic(frequency_hz,
                                            yin->frequency_hz)) {
            return false;
        }
        ++checked_notes;
    }
    return checked_notes >= 2;
}

static bool spectrum_candidate_supports_midi(
    const chord_candidate_debug_t *candidate, int midi,
    float minimum_relative, float minimum_prominence)
{
    return candidate != NULL && candidate->midi == midi &&
           candidate->relative_score >= minimum_relative &&
           candidate->prominence >= minimum_prominence;
}

static bool spectrum_supports_yin(const yin_result_t *yin,
                                  const chord_result_t *chord)
{
    if (!yin->valid || chord->debug_candidate_count <= 0 ||
        chord->independent_pitch_class_count <= 0 ||
        chord->independent_pitch_class_count >
            MUSIC_MELODY_MAX_SPECTRUM_CLASSES) {
        return false;
    }
    for (int index = 0; index < chord->debug_candidate_count; ++index) {
        if (spectrum_candidate_supports_midi(
                &chord->debug_candidates[index], yin->midi,
                MUSIC_SPECTRUM_SUPPORT_MIN_RELATIVE,
                MUSIC_SPECTRUM_SUPPORT_MIN_PROMINENCE)) {
            return true;
        }
    }
    return false;
}

static bool spectrum_supports_yin_pitch_class(
    const yin_result_t *yin, const chord_result_t *chord)
{
    if (!yin->valid || chord->debug_candidate_count <= 0 ||
        chord->independent_pitch_class_count <= 0 ||
        chord->independent_pitch_class_count >
            MUSIC_MELODY_MAX_SPECTRUM_CLASSES) {
        return false;
    }
    for (int index = 0; index < chord->debug_candidate_count; ++index) {
        const chord_candidate_debug_t *candidate =
            &chord->debug_candidates[index];
        if (candidate->midi >= 0 &&
            candidate->midi % 12 == yin->midi % 12 &&
            candidate->relative_score >=
                MUSIC_SPECTRUM_SUPPORT_MIN_RELATIVE &&
            candidate->prominence >=
                MUSIC_SPECTRUM_SUPPORT_MIN_PROMINENCE) {
            return true;
        }
    }
    return false;
}

static const chord_candidate_debug_t *find_spectrum_candidate(
    const chord_result_t *chord, int midi)
{
    for (int index = 0; index < chord->debug_candidate_count; ++index) {
        if (chord->debug_candidates[index].midi == midi) {
            return &chord->debug_candidates[index];
        }
    }
    return NULL;
}

static bool demo_polyphony_has_quality(const yin_result_t *yin,
                                       float harmonic_ratio,
                                       const chord_result_t *chord,
                                       bool selected_clipped)
{
    if (!chord->valid || chord->pitch_class_count < 2 ||
        chord->pitch_class_count > 3 || selected_clipped ||
        chord->independent_pitch_class_count < chord->pitch_class_count ||
        chord->independent_pitch_class_count >
            MUSIC_DEMO_POLY_MAX_ACTIVE_CLASSES) {
        return false;
    }

    const bool is_chord = chord->kind != CHORD_DETECTION_INTERVAL;
    const float minimum_relative = is_chord
        ? MUSIC_DEMO_CHORD_MIN_RELATIVE : MUSIC_DEMO_POLY_MIN_RELATIVE;
    const float minimum_prominence = is_chord
        ? MUSIC_DEMO_CHORD_MIN_PROMINENCE : MUSIC_DEMO_POLY_MIN_PROMINENCE;
    float weakest_relative = 1.0f;
    const chord_candidate_debug_t *accepted[3] = {0};
    for (int index = 0; index < chord->pitch_class_count; ++index) {
        const chord_candidate_debug_t *candidate =
            find_spectrum_candidate(chord, chord->midi_notes[index]);
        if (candidate == NULL || !candidate->distinct_local_peak ||
            candidate->relative_score < minimum_relative ||
            candidate->prominence < minimum_prominence) {
            return false;
        }
        accepted[index] = candidate;
        weakest_relative = fminf(weakest_relative,
                                 candidate->relative_score);
    }

    /* A very periodic single note is allowed to lose to a polyphonic result
     * only when the weaker fundamental is independently substantial. This
     * rejects the common case where a loud single note's harmonic is promoted
     * to a second note by the relaxed demo thresholds. */
    if (yin->valid && yin->confidence >= MUSIC_MELODY_STRONG_YIN_CONFIDENCE &&
        harmonic_ratio >= MUSIC_SINGLE_DOMINANCE_HARMONIC_RATIO &&
        weakest_relative <
            MUSIC_DEMO_STRONG_SINGLE_SECONDARY_RELATIVE) {
        return false;
    }

    /* Adjacent semitones at the lower end of the FFT are especially prone to
     * being the two shoulders of one Hann main lobe. Keep support for real
     * adjacent notes, but require two strong and physically separated peaks. */
    for (int left = 0; left < chord->pitch_class_count; ++left) {
        for (int right = left + 1; right < chord->pitch_class_count; ++right) {
            const int midi_distance = abs(chord->midi_notes[left] -
                                          chord->midi_notes[right]);
            if (midi_distance != 1) continue;
            if (accepted[left]->relative_score <
                    MUSIC_DEMO_ADJACENT_MIN_RELATIVE ||
                accepted[right]->relative_score <
                    MUSIC_DEMO_ADJACENT_MIN_RELATIVE ||
                accepted[left]->prominence <
                    MUSIC_DEMO_ADJACENT_MIN_PROMINENCE ||
                accepted[right]->prominence <
                    MUSIC_DEMO_ADJACENT_MIN_PROMINENCE ||
                abs(accepted[left]->peak_bin - accepted[right]->peak_bin) <
                    MUSIC_DEMO_ADJACENT_MIN_PEAK_BINS) {
                return false;
            }
        }
    }
    return true;
}

static bool correct_yin_octave_from_spectrum(yin_result_t *yin,
                                             const chord_result_t *chord)
{
    if (!yin->valid || chord->debug_candidate_count <= 0 ||
        chord->independent_pitch_class_count <= 0 ||
        chord->independent_pitch_class_count >
            MUSIC_MELODY_MAX_SPECTRUM_CLASSES) {
        return false;
    }

    /* If the spectrum contains the original octave at useful strength, YIN
     * wins. This prevents a strong second harmonic from lifting a correct G4
     * to G5. */
    for (int index = 0; index < chord->debug_candidate_count; ++index) {
        if (spectrum_candidate_supports_midi(
                &chord->debug_candidates[index], yin->midi,
                MUSIC_SPECTRUM_SUPPORT_MIN_RELATIVE,
                MUSIC_SPECTRUM_SUPPORT_MIN_PROMINENCE)) {
            return false;
        }
    }

    const chord_candidate_debug_t *best = NULL;
    float best_score = 0.0f;
    for (int index = 0; index < chord->debug_candidate_count; ++index) {
        const chord_candidate_debug_t *candidate =
            &chord->debug_candidates[index];
        const int difference = candidate->midi - yin->midi;
        if (candidate->midi < MUSIC_MELODY_FALLBACK_MIDI_MIN ||
            candidate->midi > MUSIC_MELODY_FALLBACK_MIDI_MAX ||
            difference == 0 || abs(difference) > 24 ||
            abs(difference) % 12 != 0 ||
            candidate->relative_score <
                MUSIC_OCTAVE_CORRECTION_MIN_RELATIVE ||
            candidate->prominence <
                MUSIC_OCTAVE_CORRECTION_MIN_PROMINENCE) {
            continue;
        }
        const float score = candidate->relative_score *
                            candidate->prominence;
        if (best == NULL || score > best_score) {
            best = candidate;
            best_score = score;
        }
    }
    if (best == NULL) return false;

    yin->midi = best->midi;
    if (best->peak_frequency_hz > 0.0f) {
        yin->frequency_hz = best->peak_frequency_hz;
    }
    yin->cents = note_cents_error(yin->frequency_hz, yin->midi);
    note_midi_to_name(yin->midi, yin->note_name,
                      sizeof(yin->note_name));
    return true;
}

static bool history_has_onset(const music_classifier_t *classifier,
                              music_result_type_t type, int identity,
                              bool minor)
{
    for (int index = 0; index < classifier->history_count; ++index) {
        if (classifier->type_history[index] == type &&
            classifier->identity_history[index] == identity &&
            (type != MUSIC_RESULT_CHORD ||
             classifier->minor_history[index] == minor) &&
            classifier->onset_history[index]) {
            return true;
        }
    }
    return false;
}

bool music_classifier_update(music_classifier_t *classifier,
                             const audio_frame_metrics_t *mic1_metrics,
                             const audio_frame_metrics_t *mic2_metrics,
                             float mic1_gate, float mic2_gate, int selected_mic,
                             const yin_result_t *yin_input, float harmonic_ratio,
                             const chord_result_t *chord, bool demo_profile,
                             int poly_stable_votes,
                             uint32_t timestamp_ms,
                             music_result_t *result, const char **unknown_reason)
{
    yin_result_t corrected_yin = *yin_input;
    const bool octave_corrected =
        correct_yin_octave_from_spectrum(&corrected_yin, chord);
    const yin_result_t *yin = &corrected_yin;
    if (octave_corrected) {
        harmonic_ratio = chord_detector_harmonic_explained_ratio(
            corrected_yin.frequency_hz);
    }
    memset(result, 0, sizeof(*result));
    result->octave_corrected = octave_corrected;
    result->midi = -1;
    result->timestamp_ms = timestamp_ms;
    result->mic1_rms = mic1_metrics->rms;
    result->yin_confidence = yin->confidence;
    result->harmonic_explained_ratio = harmonic_ratio;
    if (yin->valid) {
        result->frequency_hz = yin->frequency_hz;
        result->midi = yin->midi;
        result->cents = yin->cents;
        memcpy(result->note_name, yin->note_name, sizeof(result->note_name));
    }
#if MUSIC_USE_SINGLE_MIC_CH1
    (void)mic2_metrics;
    (void)mic2_gate;
    (void)selected_mic;
    result->mic2_rms = 0.0f;
    result->rms = mic1_metrics->rms;
    result->selected_mic = 1;
    const bool silence = mic1_metrics->rms < mic1_gate;
    const bool selected_above_gate = !silence;
    const bool selected_clipped = mic1_metrics->clipped;
#else
    result->mic2_rms = mic2_metrics->rms;
    const audio_frame_metrics_t *selected_metrics =
        selected_mic == 2 ? mic2_metrics : mic1_metrics;
    result->rms = selected_metrics->rms;
    result->selected_mic = selected_mic;
    const bool silence = mic1_metrics->rms < mic1_gate && mic2_metrics->rms < mic2_gate;
    const bool selected_above_gate =
        selected_metrics->rms >= (selected_mic == 2 ? mic2_gate : mic1_gate);
    const bool selected_clipped = selected_metrics->clipped;
#endif
    music_result_type_t candidate = MUSIC_RESULT_UNKNOWN;
    int identity = -1;
    bool minor = false;
    const bool yin_spectrum_single = yin->valid &&
                                     yin->confidence >= MUSIC_YIN_SPECTRAL_SINGLE_CONFIDENCE &&
                                     chord->independent_pitch_class_count == 1 &&
                                     chord->debug_candidate_count > 0 &&
                                     chord->debug_candidates[0].midi == yin->midi;
    const bool yin_harmonic_single = poly_result_is_yin_harmonics(yin, chord);
    const bool yin_spectrum_supported =
        spectrum_supports_yin(yin, chord);
    const bool yin_pitch_class_supported =
        spectrum_supports_yin_pitch_class(yin, chord);
    const melody_gate_config_t gate_config = {
        .strict_yin_confidence = MUSIC_YIN_CONFIDENCE_THRESHOLD,
        .strict_harmonic_ratio = MUSIC_SINGLE_HARMONIC_RATIO_THRESHOLD,
        .spectrum_yin_confidence = MUSIC_YIN_SPECTRAL_SINGLE_CONFIDENCE,
        .harmonic_yin_confidence = MUSIC_MELODY_YIN_OVERRIDE_CONFIDENCE,
        .strong_yin_confidence = MUSIC_MELODY_STRONG_YIN_CONFIDENCE,
        .supported_yin_confidence = MUSIC_MELODY_YIN_OVERRIDE_CONFIDENCE,
        .midi_min = MUSIC_MELODY_FALLBACK_MIDI_MIN,
        .midi_max = MUSIC_MELODY_FALLBACK_MIDI_MAX,
    };
    const melody_gate_decision_t gate = melody_gate_decide(
        &gate_config, &(melody_gate_observation_t) {
            .yin_valid = yin->valid,
            .yin_confidence = yin->confidence,
            .harmonic_ratio = harmonic_ratio,
            .midi = yin->midi,
            .signal_above_gate = selected_above_gate,
            .clipped = selected_clipped,
            .spectrum_exact_support =
                yin_spectrum_single || yin_spectrum_supported,
            .spectrum_pitch_class_support =
                yin_pitch_class_supported,
            .polyphony_is_yin_harmonics = yin_harmonic_single,
        });
    /* The demo profile may override YIN only with a genuinely independent,
     * spectrally clean polyphonic result. SNR alone is not evidence of a clean
     * spectrum: clipping, transients and one note's harmonics can all be loud. */
    const bool demo_poly_candidate = demo_profile &&
        demo_polyphony_has_quality(yin, harmonic_ratio, chord,
                                   selected_clipped);
    const bool accepted_poly = chord->valid &&
                               (!demo_profile || demo_poly_candidate);
    const bool dominant_single = !demo_poly_candidate &&
                                 (gate.dominates_polyphony ||
                                  (yin->valid &&
                                   yin->confidence >= MUSIC_SINGLE_DOMINANCE_YIN_CONFIDENCE &&
                                   harmonic_ratio >= MUSIC_SINGLE_DOMINANCE_HARMONIC_RATIO));
    const bool accepted_single = gate.accepted;
    *unknown_reason = octave_corrected
                          ? "octave_corrected_stabilizing"
                          : "low_confidence";

    if (silence) {
        candidate = MUSIC_RESULT_SILENCE;
        identity = 0;
        *unknown_reason = "silence";
#if MUSIC_USE_SINGLE_MIC_CH1
    } else if (mic1_metrics->clipped) {
#else
    } else if (selected_metrics->clipped) {
#endif
        candidate = MUSIC_RESULT_UNKNOWN;
        *unknown_reason = "clipping";
    } else if (accepted_poly && chord->kind != CHORD_DETECTION_INTERVAL && !dominant_single) {
        candidate = MUSIC_RESULT_CHORD;
        identity = chord->identity;
        minor = chord->is_minor;
    } else if (accepted_poly && chord->kind == CHORD_DETECTION_INTERVAL && !dominant_single) {
        candidate = MUSIC_RESULT_INTERVAL;
        identity = chord->identity;
    /* A real single note naturally produces 3rd/5th harmonics that occupy other
     * pitch classes. A valid high-confidence chord already won above, so do not
     * let those harmonics veto an otherwise periodic, harmonically explained note. */
    } else if (accepted_single) {
        candidate = MUSIC_RESULT_SINGLE;
        identity = yin->midi;
    } else if (yin->valid && chord->valid) {
        *unknown_reason = "single_chord_conflict";
    } else if (yin->valid && yin->confidence >= MUSIC_YIN_CONFIDENCE_THRESHOLD) {
        *unknown_reason = "low_harmonic_ratio";
    } else {
        *unknown_reason = "unstable_spectrum";
    }

    const float previous_rms = classifier->previous_rms;
    const bool amplitude_attack =
        !silence && previous_rms > 0.0f &&
        result->rms >= previous_rms * MUSIC_ONSET_RISE_RATIO &&
        result->rms - previous_rms >= MUSIC_ONSET_MIN_RMS_RISE;
    if (amplitude_attack) {
        classifier->pending_attack_ms = timestamp_ms;
    }
    classifier->previous_rms = result->rms;

    if (candidate == MUSIC_RESULT_SILENCE) {
        classifier->history_count = 0;
        classifier->history_position = 0;
        memset(classifier->onset_history, 0,
               sizeof(classifier->onset_history));
    }
    update_consecutive_candidate(classifier, candidate, identity, minor);
    const int position = classifier->history_position;
    classifier->type_history[position] = candidate;
    classifier->identity_history[position] = identity;
    classifier->minor_history[position] = minor;
    classifier->onset_history[position] = amplitude_attack;
    classifier->history_position = (position + 1) % MUSIC_STABLE_HISTORY_SIZE;
    if (classifier->history_count < MUSIC_STABLE_HISTORY_SIZE) ++classifier->history_count;

    if (!demo_profile || poly_stable_votes < MUSIC_STABLE_VOTE_COUNT ||
        poly_stable_votes > MUSIC_STABLE_HISTORY_SIZE) {
        poly_stable_votes = MUSIC_STABLE_VOTE_COUNT;
    }
    music_result_type_t stable_type = MUSIC_RESULT_UNKNOWN;
    bool holding_previous_polyphony = false;
    bool suppressing_unconfirmed_transition = false;
    if (candidate == MUSIC_RESULT_SILENCE) {
        stable_type = MUSIC_RESULT_SILENCE;
    } else if (demo_profile && candidate == MUSIC_RESULT_SINGLE &&
               classifier->consecutive_count >= MUSIC_STABLE_VOTE_COUNT) {
        stable_type = MUSIC_RESULT_SINGLE;
    } else if (demo_profile && candidate == MUSIC_RESULT_INTERVAL &&
               classifier->consecutive_count >= poly_stable_votes) {
        stable_type = MUSIC_RESULT_INTERVAL;
    } else if (demo_profile && candidate == MUSIC_RESULT_CHORD &&
               classifier->consecutive_count >=
                   MUSIC_DEMO_CHORD_CONSECUTIVE) {
        stable_type = MUSIC_RESULT_CHORD;
    } else if (!demo_profile && candidate == MUSIC_RESULT_SINGLE &&
               vote_count(classifier, candidate, identity, false) >= MUSIC_STABLE_VOTE_COUNT) {
        stable_type = MUSIC_RESULT_SINGLE;
    } else if (!demo_profile && candidate == MUSIC_RESULT_CHORD &&
               vote_count(classifier, candidate, identity, minor) >= poly_stable_votes) {
        stable_type = MUSIC_RESULT_CHORD;
    } else if (!demo_profile && candidate == MUSIC_RESULT_INTERVAL &&
               vote_count(classifier, candidate, identity, false) >= poly_stable_votes) {
        stable_type = MUSIC_RESULT_INTERVAL;
    } else if (!demo_profile && candidate == MUSIC_RESULT_UNKNOWN && classifier->has_last_emitted &&
               classifier->last_emitted.type == MUSIC_RESULT_SINGLE && yin->valid &&
               yin->midi == classifier->last_emitted.midi &&
               yin->confidence >= MUSIC_YIN_CONFIDENCE_THRESHOLD) {
        /* Once a note is stable, preserve it across a temporary FFT leakage or
         * attack/decay frame as long as YIN still sees the same confident pitch. */
        stable_type = MUSIC_RESULT_SINGLE;
    } else if (!demo_profile && candidate == MUSIC_RESULT_UNKNOWN && classifier->has_last_emitted &&
               (classifier->last_emitted.type == MUSIC_RESULT_INTERVAL ||
                classifier->last_emitted.type == MUSIC_RESULT_CHORD) &&
               vote_count(classifier, classifier->last_emitted.type,
                          result_identity(&classifier->last_emitted),
                          classifier->last_emitted.chord_is_minor) >= 2) {
        /* Preserve a stable polyphonic result over a few attack/decay frames.
         * Silence resets the history above, so this cannot latch indefinitely. */
        stable_type = classifier->last_emitted.type;
        holding_previous_polyphony = true;
        *unknown_reason = "holding_polyphonic_result";
    }
    if (demo_profile && stable_type == MUSIC_RESULT_UNKNOWN &&
        candidate != MUSIC_RESULT_SILENCE && classifier->has_last_emitted &&
        (classifier->last_emitted.type == MUSIC_RESULT_SINGLE ||
         classifier->last_emitted.type == MUSIC_RESULT_INTERVAL ||
         classifier->last_emitted.type == MUSIC_RESULT_CHORD)) {
        /* Keep the last confirmed display while a replacement is being
         * confirmed. Do not manufacture a held result here: the live YIN pitch
         * fields must remain current for the independent pitch stream. */
        suppressing_unconfirmed_transition = true;
        *unknown_reason = "holding_confirmed_result";
    }
    if (!suppressing_unconfirmed_transition &&
        stable_type == MUSIC_RESULT_UNKNOWN && candidate == MUSIC_RESULT_SINGLE) {
        *unknown_reason = "stabilizing_single";
    } else if (!suppressing_unconfirmed_transition &&
               stable_type == MUSIC_RESULT_UNKNOWN && candidate == MUSIC_RESULT_INTERVAL) {
        *unknown_reason = "stabilizing_interval";
    } else if (!suppressing_unconfirmed_transition &&
               stable_type == MUSIC_RESULT_UNKNOWN && candidate == MUSIC_RESULT_CHORD) {
        *unknown_reason = "stabilizing_chord";
    }
    result->type = stable_type;
    if (stable_type == MUSIC_RESULT_SINGLE) {
        result->confidence = gate.confidence_from_yin
                                 ? yin->confidence
                                 : fminf(yin->confidence, harmonic_ratio);
        const bool recent_attack =
            classifier->pending_attack_ms != 0 &&
            timestamp_ms - classifier->pending_attack_ms <=
                MUSIC_ONSET_ASSOCIATION_MS;
        if ((recent_attack ||
             history_has_onset(classifier, stable_type, identity, false)) &&
            (classifier->last_onset_ms == 0 ||
             timestamp_ms - classifier->last_onset_ms >=
                 MUSIC_ONSET_MIN_INTERVAL_MS)) {
            result->onset = true;
            classifier->last_onset_ms = timestamp_ms;
            classifier->pending_attack_ms = 0;
            memset(classifier->onset_history, 0,
                   sizeof(classifier->onset_history));
        }
    } else if (stable_type == MUSIC_RESULT_INTERVAL || stable_type == MUSIC_RESULT_CHORD) {
        if (holding_previous_polyphony) {
            result->confidence = classifier->last_emitted.confidence;
            result->chord_root = classifier->last_emitted.chord_root;
            result->chord_is_minor = classifier->last_emitted.chord_is_minor;
            result->pitch_class_count = classifier->last_emitted.pitch_class_count;
            memcpy(result->chord_name, classifier->last_emitted.chord_name,
                   sizeof(result->chord_name));
            memcpy(result->pitch_classes, classifier->last_emitted.pitch_classes,
                   sizeof(result->pitch_classes));
            memcpy(result->midi_notes, classifier->last_emitted.midi_notes,
                   sizeof(result->midi_notes));
            memcpy(result->chroma, classifier->last_emitted.chroma, sizeof(result->chroma));
        } else {
            result->confidence = chord->confidence;
            result->chord_root = chord->root;
            result->chord_is_minor = chord->is_minor;
            result->pitch_class_count = chord->pitch_class_count;
            memcpy(result->chord_name, chord->name, sizeof(result->chord_name));
            memcpy(result->pitch_classes, chord->pitch_classes, sizeof(result->pitch_classes));
            memcpy(result->midi_notes, chord->midi_notes, sizeof(result->midi_notes));
            memcpy(result->chroma, chord->chroma, sizeof(result->chroma));
        }
    }
    const bool emit = !holding_previous_polyphony &&
                      !suppressing_unconfirmed_transition &&
                      (result->onset ||
                      (!classifier->has_last_emitted ||
                       changed(result, &classifier->last_emitted) ||
                       timestamp_ms - classifier->last_emit_ms >=
                           MUSIC_RESULT_REPEAT_INTERVAL_MS));
    if (emit) {
        classifier->last_emitted = *result;
        classifier->last_emit_ms = timestamp_ms;
        classifier->has_last_emitted = true;
    }
    return emit;
}
