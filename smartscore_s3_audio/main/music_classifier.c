#include "music_classifier.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
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
                             const chord_result_t *chord, uint32_t timestamp_ms,
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
#else
    result->mic2_rms = mic2_metrics->rms;
    const audio_frame_metrics_t *selected_metrics =
        selected_mic == 2 ? mic2_metrics : mic1_metrics;
    result->rms = selected_metrics->rms;
    result->selected_mic = selected_mic;
    const bool silence = mic1_metrics->rms < mic1_gate && mic2_metrics->rms < mic2_gate;
#endif
    music_result_type_t candidate = MUSIC_RESULT_UNKNOWN;
    int identity = -1;
    bool minor = false;
    const bool strong_single = yin->valid &&
                               yin->confidence >= MUSIC_YIN_CONFIDENCE_THRESHOLD &&
                               harmonic_ratio >= MUSIC_SINGLE_HARMONIC_RATIO_THRESHOLD;
    const bool yin_spectrum_single = yin->valid &&
                                     yin->confidence >= MUSIC_YIN_SPECTRAL_SINGLE_CONFIDENCE &&
                                     chord->independent_pitch_class_count == 1 &&
                                     chord->debug_candidate_count > 0 &&
                                     chord->debug_candidates[0].midi == yin->midi;
    const bool yin_harmonic_single = poly_result_is_yin_harmonics(yin, chord);
    const bool yin_spectrum_supported = spectrum_supports_yin(yin, chord);
    const bool yin_continuity_fallback =
        yin->valid && !chord->valid &&
        yin->confidence >= MUSIC_MELODY_YIN_FALLBACK_CONFIDENCE &&
        yin->midi >= MUSIC_MELODY_FALLBACK_MIDI_MIN &&
        yin->midi <= MUSIC_MELODY_FALLBACK_MIDI_MAX &&
        yin_spectrum_supported;
    const bool dominant_single = yin_spectrum_single ||
                                 yin_harmonic_single ||
                                 (yin->valid &&
                                  yin->confidence >= MUSIC_SINGLE_DOMINANCE_YIN_CONFIDENCE &&
                                  harmonic_ratio >= MUSIC_SINGLE_DOMINANCE_HARMONIC_RATIO);
    const bool accepted_single = strong_single || yin_spectrum_single ||
                                 yin_harmonic_single || yin_continuity_fallback;
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
    } else if (chord->valid && chord->kind != CHORD_DETECTION_INTERVAL && !dominant_single) {
        candidate = MUSIC_RESULT_CHORD;
        identity = chord->identity;
        minor = chord->is_minor;
    } else if (chord->valid && chord->kind == CHORD_DETECTION_INTERVAL && !dominant_single) {
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
    const int position = classifier->history_position;
    classifier->type_history[position] = candidate;
    classifier->identity_history[position] = identity;
    classifier->minor_history[position] = minor;
    classifier->onset_history[position] = amplitude_attack;
    classifier->history_position = (position + 1) % MUSIC_STABLE_HISTORY_SIZE;
    if (classifier->history_count < MUSIC_STABLE_HISTORY_SIZE) ++classifier->history_count;

    music_result_type_t stable_type = MUSIC_RESULT_UNKNOWN;
    bool holding_previous_polyphony = false;
    if (candidate == MUSIC_RESULT_SILENCE) {
        stable_type = MUSIC_RESULT_SILENCE;
    } else if (candidate == MUSIC_RESULT_SINGLE &&
               vote_count(classifier, candidate, identity, false) >= MUSIC_STABLE_VOTE_COUNT) {
        stable_type = MUSIC_RESULT_SINGLE;
    } else if (candidate == MUSIC_RESULT_CHORD &&
               vote_count(classifier, candidate, identity, minor) >= MUSIC_STABLE_VOTE_COUNT) {
        stable_type = MUSIC_RESULT_CHORD;
    } else if (candidate == MUSIC_RESULT_INTERVAL &&
               vote_count(classifier, candidate, identity, false) >= MUSIC_STABLE_VOTE_COUNT) {
        stable_type = MUSIC_RESULT_INTERVAL;
    } else if (candidate == MUSIC_RESULT_UNKNOWN && classifier->has_last_emitted &&
               classifier->last_emitted.type == MUSIC_RESULT_SINGLE && yin->valid &&
               yin->midi == classifier->last_emitted.midi &&
               yin->confidence >= MUSIC_YIN_CONFIDENCE_THRESHOLD) {
        /* Once a note is stable, preserve it across a temporary FFT leakage or
         * attack/decay frame as long as YIN still sees the same confident pitch. */
        stable_type = MUSIC_RESULT_SINGLE;
    } else if (candidate == MUSIC_RESULT_UNKNOWN && classifier->has_last_emitted &&
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
    if (stable_type == MUSIC_RESULT_UNKNOWN && candidate == MUSIC_RESULT_SINGLE) {
        *unknown_reason = "stabilizing_single";
    } else if (stable_type == MUSIC_RESULT_UNKNOWN && candidate == MUSIC_RESULT_INTERVAL) {
        *unknown_reason = "stabilizing_interval";
    } else if (stable_type == MUSIC_RESULT_UNKNOWN && candidate == MUSIC_RESULT_CHORD) {
        *unknown_reason = "stabilizing_chord";
    }
    result->type = stable_type;
    if (stable_type == MUSIC_RESULT_SINGLE) {
        result->confidence = (yin_continuity_fallback ||
                              (yin_spectrum_single && !strong_single)) ?
            yin->confidence : fminf(yin->confidence, harmonic_ratio);
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
