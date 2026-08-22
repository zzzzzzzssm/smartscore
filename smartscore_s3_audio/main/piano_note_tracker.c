#include "piano_note_tracker.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "music_detector_config.h"
#include "note_utils.h"

static const char *TAG = "PIANO_TRACKER";

static float clamp01(float value)
{
    return fmaxf(0.0f, fminf(1.0f, value));
}

static void add_yin_evidence(float evidence[CHORD_PIANO_KEY_COUNT],
                             const yin_result_t *yin,
                             float minimum_confidence)
{
    if (yin == NULL || !yin->valid || yin->confidence < minimum_confidence ||
        yin->midi < PIANO_TRACKER_MIDI_MIN ||
        yin->midi > PIANO_TRACKER_MIDI_MAX) {
        return;
    }
    const int key = yin->midi - PIANO_TRACKER_MIDI_MIN;
    const float supported = 0.55f + 0.45f * clamp01(yin->confidence);
    evidence[key] = fmaxf(evidence[key], supported);
}

static bool virtual_low_confirmed(const chord_result_t *spectrum, int key,
                                  const yin_result_t *low_yin)
{
    if (!spectrum->key_uses_virtual_fundamental[key] || low_yin == NULL ||
        !low_yin->valid ||
        low_yin->confidence < PIANO_TRACKER_LOW_YIN_CONFIDENCE) {
        return false;
    }
    return low_yin->midi == PIANO_TRACKER_MIDI_MIN + key;
}

static bool strict_low_yin_anchor(const yin_result_t *low_yin)
{
    return low_yin != NULL && low_yin->valid &&
        low_yin->confidence >= MUSIC_LOW_CHORD_YIN_CONFIDENCE &&
        low_yin->midi >= PIANO_TRACKER_MIDI_MIN &&
        low_yin->midi <= PIANO_TRACKER_LOW_TAIL_MIDI_MAX;
}

static bool strict_virtual_low_confirmed(
    const chord_result_t *spectrum, int key, const yin_result_t *low_yin)
{
    return strict_low_yin_anchor(low_yin) &&
        spectrum->key_uses_virtual_fundamental[key] &&
        low_yin->midi == PIANO_TRACKER_MIDI_MIN + key;
}

static bool low_yin_sustains_key(const yin_result_t *low_yin, int key)
{
    return low_yin != NULL && low_yin->valid &&
        low_yin->confidence >= PIANO_TRACKER_LOW_YIN_SUSTAIN_CONFIDENCE &&
        low_yin->midi == PIANO_TRACKER_MIDI_MIN + key;
}

static bool strong_virtual_low(const chord_result_t *spectrum, int key)
{
    const int midi = PIANO_TRACKER_MIDI_MIN + key;
    return midi <= PIANO_TRACKER_LOW_TAIL_MIDI_MAX &&
        spectrum->key_uses_virtual_fundamental[key] &&
        spectrum->key_salience[key] >=
            PIANO_TRACKER_LOW_VIRTUAL_SALIENCE &&
        spectrum->key_fundamental_prominence[key] >=
            PIANO_TRACKER_LOW_VIRTUAL_PROMINENCE;
}

static int strongest_virtual_low_key(const piano_note_tracker_t *tracker,
                                     const chord_result_t *spectrum,
                                     const yin_result_t *low_yin)
{
    int strongest = -1;
    const bool anchored = strict_low_yin_anchor(low_yin);
    for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
        if (tracker->low_rearm_blocked[key] ||
            !strong_virtual_low(spectrum, key)) {
            continue;
        }
        if (anchored && low_yin->midi != PIANO_TRACKER_MIDI_MIN + key) {
            continue;
        }
        if (strongest < 0 || spectrum->key_salience[key] >
                             spectrum->key_salience[strongest]) {
            strongest = key;
        }
    }
    return strongest;
}

static bool has_fundamental_support(const chord_result_t *spectrum, int key,
                                    const yin_result_t *low_yin)
{
    return spectrum->key_has_independent_fundamental[key] ||
           virtual_low_confirmed(spectrum, key, low_yin);
}

static bool single_candidate_confident(
    const chord_result_t *spectrum, const yin_result_t *yin, int key,
    const yin_result_t *low_yin, float harmonic_explained_ratio)
{
    if (key < 0 || key >= CHORD_PIANO_KEY_COUNT || yin == NULL ||
        !yin->valid || yin->midi != PIANO_TRACKER_MIDI_MIN + key ||
        yin->confidence < PIANO_TRACKER_SINGLE_MIN_CONFIDENCE ||
        !has_fundamental_support(spectrum, key, low_yin) ||
        spectrum->key_salience[key] < PIANO_TRACKER_SINGLE_SALIENCE ||
        spectrum->key_fundamental_prominence[key] <
            PIANO_TRACKER_SINGLE_PROMINENCE) {
        return false;
    }
    const float minimum_harmonic_ratio = yin->midi == 60
        ? PIANO_TRACKER_SINGLE_C4_HARMONIC_RATIO
        : (yin->midi <= PIANO_TRACKER_LOW_TAIL_MIDI_MAX
               ? PIANO_TRACKER_SINGLE_LOW_HARMONIC_RATIO
               : PIANO_TRACKER_SINGLE_HIGH_HARMONIC_RATIO);
    return harmonic_explained_ratio >= minimum_harmonic_ratio;
}

static const yin_result_t *select_dominant_yin(
    const chord_result_t *spectrum, const yin_result_t *high_yin,
    const yin_result_t *low_yin)
{
    const bool high_valid = high_yin != NULL && high_yin->valid;
    bool low_valid = low_yin != NULL && low_yin->valid &&
                     low_yin->midi >= PIANO_TRACKER_MIDI_MIN &&
                     low_yin->midi <= PIANO_TRACKER_LOW_TAIL_MIDI_MAX;
    if (low_valid) {
        const int low_key = low_yin->midi - PIANO_TRACKER_MIDI_MIN;
        low_valid = has_fundamental_support(spectrum, low_key, low_yin);
    }
    if (low_valid && (!high_valid ||
                      low_yin->confidence >= high_yin->confidence)) {
        return low_yin;
    }
    return high_valid ? high_yin : NULL;
}

static bool has_trusted_independent_fundamental(
    const chord_result_t *spectrum, int key, const yin_result_t *low_yin)
{
    return has_fundamental_support(spectrum, key, low_yin) &&
           spectrum->key_salience[key] >=
               PIANO_TRACKER_INDEPENDENT_SALIENCE &&
           spectrum->key_fundamental_prominence[key] >=
               PIANO_TRACKER_INDEPENDENT_PROMINENCE;
}

static bool is_secondary_candidate(const chord_result_t *spectrum, int key,
                                   int dominant_key,
                                   const yin_result_t *low_yin)
{
    if (key < 0 || key >= CHORD_PIANO_KEY_COUNT || key == dominant_key) {
        return false;
    }
    int distance = key - dominant_key;
    if (distance < 0) distance = -distance;
    /* A relaxed persistent octave would reopen the exact false-octave bug
     * this tracker was introduced to prevent. Real octaves continue through
     * the strict two-independent-fundamental path. */
    if (distance > 0 && distance % 12 == 0) return false;
    return has_fundamental_support(spectrum, key, low_yin) &&
           spectrum->key_salience[key] >=
               PIANO_TRACKER_SECONDARY_SALIENCE &&
           spectrum->key_fundamental_prominence[key] >=
               PIANO_TRACKER_SECONDARY_PROMINENCE;
}

static bool reliable_interval_keys(const chord_result_t *spectrum,
                                   int keys[2])
{
    if (!spectrum->valid ||
        spectrum->kind != CHORD_DETECTION_INTERVAL ||
        spectrum->pitch_class_count != 2 ||
        spectrum->confidence < PIANO_TRACKER_POLY_INTERVAL_CONFIDENCE) {
        return false;
    }
    for (int index = 0; index < 2; ++index) {
        const int midi = spectrum->midi_notes[index];
        if (midi < PIANO_TRACKER_MIDI_MIN ||
            midi > PIANO_TRACKER_MIDI_MAX) {
            return false;
        }
        const int key = midi - PIANO_TRACKER_MIDI_MIN;
        if (!spectrum->key_has_independent_fundamental[key] ||
            spectrum->key_salience[key] < PIANO_TRACKER_POLY_SALIENCE ||
            spectrum->key_fundamental_prominence[key] <
                PIANO_TRACKER_POLY_PROMINENCE) {
            return false;
        }
        keys[index] = key;
    }
    return keys[0] != keys[1];
}

static int align_dominant_key_to_interval(const chord_result_t *spectrum,
                                          const yin_result_t *dominant_yin,
                                          int original_key)
{
    if (dominant_yin == NULL || !dominant_yin->valid ||
        dominant_yin->frequency_hz <= 0.0f) {
        return original_key;
    }
    int interval_keys[2];
    if (!reliable_interval_keys(spectrum, interval_keys)) {
        return original_key;
    }
    int best_key = original_key;
    float best_cents = PIANO_TRACKER_INTERVAL_ALIGN_MAX_CENTS + 1.0f;
    for (int index = 0; index < 2; ++index) {
        const int key = interval_keys[index];
        const int midi = PIANO_TRACKER_MIDI_MIN + key;
        int semitone_distance = midi - dominant_yin->midi;
        if (semitone_distance < 0) semitone_distance = -semitone_distance;
        if (semitone_distance > 1) continue;
        const float cents = fabsf(note_cents_error(
            dominant_yin->frequency_hz, midi));
        if (cents <= PIANO_TRACKER_INTERVAL_ALIGN_MAX_CENTS &&
            cents < best_cents) {
            best_key = key;
            best_cents = cents;
        }
    }
    return best_key;
}

static bool interval_contains_key(const int interval_keys[2], int key)
{
    return interval_keys[0] == key || interval_keys[1] == key;
}

static void sort_midi_notes(int notes[PIANO_NOTE_SET_MAX_KEYS],
                            uint8_t count)
{
    for (uint8_t index = 1; index < count; ++index) {
        const int value = notes[index];
        uint8_t move = index;
        while (move > 0 && notes[move - 1] > value) {
            notes[move] = notes[move - 1];
            --move;
        }
        notes[move] = value;
    }
}

static bool midi_sets_equal(const int first[PIANO_NOTE_SET_MAX_KEYS],
                            uint8_t first_count,
                            const int second[PIANO_NOTE_SET_MAX_KEYS],
                            uint8_t second_count)
{
    if (first_count != second_count) return false;
    for (uint8_t index = 0; index < first_count; ++index) {
        if (first[index] != second[index]) return false;
    }
    return true;
}

static uint32_t key_event_time(const piano_note_tracker_t *tracker, int key);

static bool poly_notes_share_onset(
    const piano_note_tracker_t *tracker,
    const chord_result_t *spectrum, const yin_result_t *low_yin,
    const int notes[PIANO_NOTE_SET_MAX_KEYS], uint8_t count)
{
    uint32_t earliest = UINT32_MAX;
    uint32_t latest = 0;
    for (uint8_t index = 0; index < count; ++index) {
        const int key = notes[index] - PIANO_TRACKER_MIDI_MIN;
        const uint32_t onset = key_event_time(tracker, key);
        if (onset == 0) {
            /* Low YIN can become confident one frame after the physical
             * partials began, so its virtual anchor may not have crossed the
             * spectral onset threshold. The other, physical chord tones must
             * still carry coherent onset times. */
            if (strict_virtual_low_confirmed(spectrum, key, low_yin)) continue;
            return false;
        }
        if (onset < earliest) earliest = onset;
        if (onset > latest) latest = onset;
    }
    return earliest != UINT32_MAX &&
        latest - earliest <= PIANO_TRACKER_CO_ONSET_MS;
}

static bool low_periodicity_supports_midi(
    const yin_result_t *low_yin, const low_note_result_t *low_notes, int midi)
{
    if (low_yin != NULL && low_yin->valid && low_yin->midi == midi &&
        low_yin->confidence >= PIANO_TRACKER_LOW_YIN_CONFIDENCE) {
        return true;
    }
    if (low_notes == NULL || !low_notes->valid) return false;
    for (uint8_t index = 0; index < low_notes->count; ++index) {
        if (low_notes->midi[index] == midi &&
            low_notes->confidence[index] >=
                MUSIC_LOW_MATCH_SECONDARY_CONFIDENCE) {
            return true;
        }
    }
    return false;
}

static const chord_candidate_debug_t *find_debug_candidate(
    const chord_result_t *spectrum, int midi)
{
    for (int index = 0; index < spectrum->debug_candidate_count; ++index) {
        if (spectrum->debug_candidates[index].midi == midi) {
            return &spectrum->debug_candidates[index];
        }
    }
    return NULL;
}

static bool spectrum_contains_exact_midi(const chord_result_t *spectrum,
                                         int midi)
{
    if (spectrum == NULL || !spectrum->valid) return false;
    for (int index = 0; index < spectrum->pitch_class_count; ++index) {
        if (spectrum->midi_notes[index] == midi) return true;
    }
    return false;
}

static bool low_poly_has_periodicity_anchor(
    const chord_result_t *spectrum, const yin_result_t *low_yin,
    const low_note_result_t *low_notes)
{
    if (spectrum == NULL || !spectrum->valid ||
        spectrum->pitch_class_count < 2) {
        return false;
    }
    for (int index = 0; index < spectrum->pitch_class_count; ++index) {
        const int midi = spectrum->midi_notes[index];
        if (midi < MUSIC_FFT_POLY_PRIORITY_MIN_MIDI &&
            low_periodicity_supports_midi(low_yin, low_notes, midi)) {
            return true;
        }
    }
    return false;
}

static bool high_fft_poly_has_quality(
    const chord_result_t *spectrum,
    const int notes[PIANO_NOTE_SET_MAX_KEYS], uint8_t count, bool fast)
{
    if (count < 2) return false;
    const float minimum_relative = fast
        ? MUSIC_HIGH_FFT_FAST_MIN_RELATIVE
        : MUSIC_HIGH_FFT_MIN_RELATIVE;
    const float minimum_prominence = fast
        ? MUSIC_HIGH_FFT_FAST_MIN_PROMINENCE
        : MUSIC_HIGH_FFT_MIN_PROMINENCE;
    const float minimum_confidence = count >= 3
        ? MUSIC_HIGH_FFT_FAST_CHORD_CONFIDENCE
        : MUSIC_HIGH_FFT_FAST_INTERVAL_CONFIDENCE;
    if (fast && spectrum->confidence < minimum_confidence) return false;

    for (uint8_t index = 0; index < count; ++index) {
        const int midi = notes[index];
        if (midi < MUSIC_FFT_POLY_PRIORITY_MIN_MIDI ||
            midi > PIANO_TRACKER_MIDI_MAX) {
            return false;
        }
        const int key = midi - PIANO_TRACKER_MIDI_MIN;
        const chord_candidate_debug_t *candidate =
            find_debug_candidate(spectrum, midi);
        if (!spectrum->key_has_independent_fundamental[key] ||
            candidate == NULL || !candidate->distinct_local_peak ||
            candidate->relative_score < minimum_relative ||
            candidate->prominence < minimum_prominence) {
            return false;
        }
    }
    return true;
}

static bool midi_is_harmonic_of(int upper_midi, int source_midi)
{
    if (upper_midi <= source_midi) return false;
    const float ratio = note_midi_to_frequency(upper_midi) /
        note_midi_to_frequency(source_midi);
    const int harmonic = (int)lrintf(ratio);
    if (harmonic < 2 || harmonic > PIANO_TRACKER_POLY_HARMONIC_MAX) {
        return false;
    }
    const float cents = 1200.0f * log2f(ratio / (float)harmonic);
    return fabsf(cents) <= PIANO_TRACKER_POLY_HARMONIC_MAX_CENTS;
}

static bool is_single_low_harmonic_family(
    const chord_result_t *spectrum,
    const int notes[PIANO_NOTE_SET_MAX_KEYS], uint8_t count)
{
    /* A major/minor triad whose three "notes" are all integer harmonics of
     * one stronger low physical peak is a single piano note, not polyphony.
     * Keep two-note intervals out of this veto so real C5+F5 remains valid. */
    if (count < 3) return false;
    for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
        const int source_midi = PIANO_TRACKER_MIDI_MIN + key;
        if (source_midi > PIANO_TRACKER_LOW_TAIL_MIDI_MAX ||
            !spectrum->key_has_independent_fundamental[key] ||
            spectrum->key_salience[key] <
                PIANO_TRACKER_POLY_HARMONIC_SOURCE_SALIENCE ||
            spectrum->key_fundamental_prominence[key] <
                PIANO_TRACKER_POLY_HARMONIC_SOURCE_PROMINENCE) {
            continue;
        }
        bool source_is_candidate = false;
        bool all_harmonics = true;
        for (uint8_t index = 0; index < count; ++index) {
            if (notes[index] == source_midi) {
                source_is_candidate = true;
                break;
            }
            if (!midi_is_harmonic_of(notes[index], source_midi)) {
                all_harmonics = false;
                break;
            }
        }
        if (!source_is_candidate && all_harmonics) return true;
    }
    return false;
}

static bool build_poly_candidate(const piano_note_tracker_t *tracker,
                                 const chord_result_t *spectrum,
                                 const yin_result_t *low_yin,
                                 const low_note_result_t *low_notes,
                                 int notes[PIANO_NOTE_SET_MAX_KEYS],
                                 uint8_t *count,
                                 bool *direct_low_match,
                                 bool *high_fft_priority,
                                 bool *fast_high_fft)
{
    *direct_low_match = false;
    *high_fft_priority = false;
    *fast_high_fft = false;
    if (low_notes != NULL && low_notes->valid && low_notes->count >= 2U) {
        const uint8_t matched_count = low_notes->count > PIANO_NOTE_SET_MAX_KEYS
            ? PIANO_NOTE_SET_MAX_KEYS : low_notes->count;
        bool yin_anchors_match = false;
        for (uint8_t index = 0; index < matched_count; ++index) {
            if (low_notes->midi[index] < PIANO_TRACKER_MIDI_MIN ||
                low_notes->midi[index] > PIANO_TRACKER_LOW_TAIL_MIDI_MAX ||
                low_notes->confidence[index] <
                    MUSIC_LOW_MATCH_SECONDARY_CONFIDENCE) {
                return false;
            }
            notes[index] = low_notes->midi[index];
            if (low_yin != NULL && low_yin->valid &&
                low_yin->confidence >= PIANO_TRACKER_LOW_YIN_CONFIDENCE &&
                low_yin->midi == notes[index]) {
                yin_anchors_match = true;
            }
        }
        if (!yin_anchors_match) return false;
        sort_midi_notes(notes, matched_count);
        for (uint8_t index = 1; index < matched_count; ++index) {
            if (notes[index] == notes[index - 1]) return false;
        }
        *count = matched_count;
        *direct_low_match = true;
        return true;
    }
    if (!spectrum->valid) return false;
    uint8_t expected_count = 0;
    float minimum_confidence = 1.0f;
    if (spectrum->kind == CHORD_DETECTION_INTERVAL &&
        spectrum->pitch_class_count == 2) {
        expected_count = 2;
        minimum_confidence = PIANO_TRACKER_POLY_INTERVAL_CONFIDENCE;
    } else if ((spectrum->kind == CHORD_DETECTION_MAJOR ||
                spectrum->kind == CHORD_DETECTION_MINOR) &&
               spectrum->pitch_class_count == 3) {
        expected_count = 3;
        minimum_confidence = PIANO_TRACKER_POLY_CHORD_CONFIDENCE;
    } else {
        return false;
    }
    if (spectrum->confidence < minimum_confidence) return false;

    uint8_t virtual_low_count = 0;
    for (uint8_t index = 0; index < expected_count; ++index) {
        const int midi = spectrum->midi_notes[index];
        if (midi < PIANO_TRACKER_MIDI_MIN ||
            midi > PIANO_TRACKER_MIDI_MAX) {
            return false;
        }
        const int key = midi - PIANO_TRACKER_MIDI_MIN;
        const bool independent =
            spectrum->key_has_independent_fundamental[key];
        const bool anchored_virtual = !independent &&
            strict_virtual_low_confirmed(spectrum, key, low_yin);
        if (anchored_virtual && ++virtual_low_count > 1) return false;
        const bool note_has_required_fundamental = independent ||
            anchored_virtual;
        if (!note_has_required_fundamental ||
            spectrum->key_salience[key] < PIANO_TRACKER_POLY_SALIENCE ||
            spectrum->key_fundamental_prominence[key] <
                PIANO_TRACKER_POLY_PROMINENCE) {
            return false;
        }
        notes[index] = midi;
    }
    sort_midi_notes(notes, expected_count);
    for (uint8_t index = 1; index < expected_count; ++index) {
        if (notes[index] == notes[index - 1]) return false;
    }
    if (is_single_low_harmonic_family(spectrum, notes, expected_count)) {
        return false;
    }

    /* Below C4, at least one chord tone must be confirmed by a periodicity
     * detector. The FFT still supplies the remaining physical fundamentals,
     * but it cannot establish a low chord from broad, closely spaced bins on
     * its own. */
    bool contains_low_note = false;
    bool low_periodicity_anchor = false;
    for (uint8_t index = 0; index < expected_count; ++index) {
        if (notes[index] >= MUSIC_FFT_POLY_PRIORITY_MIN_MIDI) continue;
        contains_low_note = true;
        if (low_periodicity_supports_midi(low_yin, low_notes, notes[index])) {
            low_periodicity_anchor = true;
        }
    }
    if (contains_low_note && !low_periodicity_anchor) return false;

    *high_fft_priority = high_fft_poly_has_quality(
        spectrum, notes, expected_count, false);
    *fast_high_fft = *high_fft_priority && high_fft_poly_has_quality(
        spectrum, notes, expected_count, true);
    if (!*high_fft_priority && !poly_notes_share_onset(
            tracker, spectrum, low_yin, notes, expected_count)) {
        return false;
    }
    *count = expected_count;
    return true;
}

static void clear_poly_candidate(piano_note_tracker_t *tracker)
{
    tracker->poly_candidate_count = 0;
    tracker->poly_candidate_frames = 0;
    tracker->poly_candidate_missing_frames = 0;
    for (uint8_t index = 0; index < PIANO_NOTE_SET_MAX_KEYS; ++index) {
        tracker->poly_candidate_midi[index] = -1;
    }
}

static bool spectrum_supports_locked_poly(
    const piano_note_tracker_t *tracker, const chord_result_t *spectrum)
{
    if (!tracker->poly_lock_active || tracker->poly_locked_count < 2) {
        return false;
    }
    for (uint8_t note = 0; note < tracker->poly_locked_count; ++note) {
        bool supported = false;
        for (int candidate = 0;
             candidate < spectrum->debug_candidate_count; ++candidate) {
            const chord_candidate_debug_t *peak =
                &spectrum->debug_candidates[candidate];
            if (peak->midi == tracker->poly_locked_midi[note] &&
                peak->distinct_local_peak &&
                peak->relative_score >=
                    MUSIC_VIRTUAL_ROOT_HOLD_MIN_RELATIVE &&
                peak->prominence >=
                    MUSIC_VIRTUAL_ROOT_HOLD_MIN_PROMINENCE) {
                supported = true;
                break;
            }
        }
        if (!supported) return false;
    }
    return true;
}

/* Returns true while a confirmed polyphonic set should own the exact-key
 * output. Intervals need two consecutive clean frames and triads need three;
 * a locked set tolerates one missing frame so one FFT wobble cannot blank P4. */
static bool update_poly_lock(piano_note_tracker_t *tracker,
                             const chord_result_t *spectrum,
                             const yin_result_t *low_yin,
                             const low_note_result_t *low_notes,
                             bool signal_active, bool allow_attack)
{
    int notes[PIANO_NOTE_SET_MAX_KEYS] = {-1, -1, -1, -1};
    uint8_t count = 0;
    bool direct_low_match = false;
    bool high_fft_priority = false;
    bool fast_high_fft = false;
    const bool valid = signal_active && allow_attack &&
        build_poly_candidate(tracker, spectrum, low_yin, low_notes, notes,
                             &count, &direct_low_match, &high_fft_priority,
                             &fast_high_fft);
    if (valid && tracker->poly_lock_active &&
        midi_sets_equal(notes, count, tracker->poly_locked_midi,
                        tracker->poly_locked_count)) {
        tracker->poly_missing_frames = 0;
        clear_poly_candidate(tracker);
        return true;
    }

    if (valid) {
        if (midi_sets_equal(notes, count, tracker->poly_candidate_midi,
                            tracker->poly_candidate_count)) {
            tracker->poly_candidate_missing_frames = 0;
            if (tracker->poly_candidate_frames < UINT8_MAX) {
                ++tracker->poly_candidate_frames;
            }
        } else {
            memcpy(tracker->poly_candidate_midi, notes, sizeof(notes));
            tracker->poly_candidate_count = count;
            tracker->poly_candidate_frames = 1;
            tracker->poly_candidate_missing_frames = 0;
        }
        const uint8_t required_frames = direct_low_match ? 1U
            : (fast_high_fft && count == 2 ? 1U
               : (fast_high_fft && count >= 3 ? 2U
                  : (count >= 3 ? PIANO_TRACKER_CHORD_STABLE_FRAMES
                                : PIANO_TRACKER_POLY_STABLE_FRAMES)));
        if (tracker->poly_candidate_frames >= required_frames) {
            const bool changed = !tracker->poly_lock_active ||
                !midi_sets_equal(notes, count, tracker->poly_locked_midi,
                                 tracker->poly_locked_count);
            memcpy(tracker->poly_locked_midi, notes, sizeof(notes));
            tracker->poly_locked_count = count;
            tracker->poly_lock_active = true;
            tracker->poly_missing_frames = 0;
            clear_poly_candidate(tracker);
            if (changed) {
                ESP_LOGI(TAG,
                         "stable poly lock count=%u midi=[%d,%d,%d] source=%s fast=%s",
                         (unsigned)count, notes[0], notes[1], notes[2],
                         direct_low_match ? "yin-low"
                             : (high_fft_priority ? "fft-high" : "hybrid"),
                         fast_high_fft ? "yes" : "no");
            }
            return true;
        }
    } else if (tracker->poly_candidate_count > 0 &&
               tracker->poly_candidate_missing_frames <
                   PIANO_TRACKER_POLY_CANDIDATE_MISS_FRAMES) {
        ++tracker->poly_candidate_missing_frames;
    } else {
        clear_poly_candidate(tracker);
    }

    if (tracker->poly_lock_active &&
        spectrum->virtual_root_quarantined && signal_active &&
        spectrum_supports_locked_poly(tracker, spectrum)) {
        /* A virtual root is most likely to leak during the one or two frames
         * in which the weaker upper peak falls just below the full interval
         * classifier. Keep the already verified physical pair while both
         * local peaks remain visible; silence still releases normally. */
        tracker->poly_missing_frames = 0;
        return true;
    }

    if (tracker->poly_lock_active) {
        if (tracker->poly_missing_frames < UINT8_MAX) {
            ++tracker->poly_missing_frames;
        }
        if (tracker->poly_missing_frames <
            PIANO_TRACKER_POLY_RELEASE_FRAMES) {
            return true;
        }
        ESP_LOGI(TAG, "stable poly released missing=%u",
                 (unsigned)tracker->poly_missing_frames);
        tracker->poly_lock_active = false;
        tracker->poly_locked_count = 0;
        tracker->poly_missing_frames = 0;
        for (uint8_t index = 0; index < PIANO_NOTE_SET_MAX_KEYS; ++index) {
            tracker->poly_locked_midi[index] = -1;
        }
    }
    return false;
}

static void update_key_event_envelopes(
    piano_note_tracker_t *tracker, const chord_result_t *spectrum,
    const yin_result_t *low_yin, bool signal_active, float rms,
    uint32_t timestamp_ms)
{
    memset(tracker->fresh_onset, 0, sizeof(tracker->fresh_onset));
    const bool global_attack = signal_active && tracker->previous_rms > 0.0f &&
        rms >= tracker->previous_rms * MUSIC_ONSET_RISE_RATIO &&
        rms - tracker->previous_rms >= MUSIC_ONSET_MIN_RMS_RISE;
    tracker->previous_rms = rms;

    int interval_keys[2] = {-1, -1};
    const bool reliable_interval = reliable_interval_keys(
        spectrum, interval_keys);
    for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
        const float raw = signal_active ? spectrum->key_salience[key] : 0.0f;
        const float previous = tracker->previous_evidence[key];
        if (tracker->low_rearm_blocked[key]) {
            if (raw < PIANO_TRACKER_RELEASE_THRESHOLD) {
                if (tracker->low_rearm_quiet_frames[key] < UINT8_MAX) {
                    ++tracker->low_rearm_quiet_frames[key];
                }
                if (tracker->low_rearm_quiet_frames[key] >=
                    PIANO_TRACKER_LOW_REARM_QUIET_FRAMES) {
                    tracker->low_rearm_blocked[key] = false;
                    tracker->low_rearm_quiet_frames[key] = 0;
                }
            } else {
                tracker->low_rearm_quiet_frames[key] = 0;
            }
        }
        const bool fundamental = signal_active &&
            has_fundamental_support(spectrum, key, low_yin);
        const bool crossed_support =
            previous < PIANO_TRACKER_RELEASE_THRESHOLD &&
            raw >= PIANO_TRACKER_SECONDARY_SALIENCE;
        const bool rose = raw >= PIANO_TRACKER_SECONDARY_SALIENCE &&
            raw >= previous * PIANO_TRACKER_ONSET_RISE_RATIO &&
            raw - previous >= PIANO_TRACKER_ONSET_MIN_RISE;
        const bool interval_attack = reliable_interval && global_attack &&
            interval_contains_key(interval_keys, key) &&
            !tracker->active[key] &&
            !tracker->decay_tail[key];
        const bool was_tail = tracker->decay_tail[key];
        /* A held key may breathe slightly as the spectrum moves, but that is
         * not a new piano event. Only an inactive key, or an active key which
         * had already entered its decay tail, may open a fresh onset. */
        const bool fresh = fundamental &&
            !tracker->low_rearm_blocked[key] &&
            (!tracker->active[key] || was_tail) &&
            (was_tail
                 ? rose
                 : (crossed_support || rose || interval_attack));
        tracker->fresh_onset[key] = fresh;
        if (fresh) {
            const bool new_event =
                tracker->candidate_onset_ms[key] == 0 || was_tail;
            if (new_event) {
                tracker->candidate_onset_ms[key] = timestamp_ms;
            }
            tracker->falling_frames[key] = 0;
            tracker->decay_tail[key] = false;
            tracker->low_rearm_quiet_frames[key] = 0;
            tracker->peak_evidence[key] = new_event
                ? raw : fmaxf(tracker->peak_evidence[key], raw);
            if (was_tail && tracker->active[key]) {
                tracker->onset_ms[key] = timestamp_ms;
                tracker->release_frames[key] = 0;
            }
        } else if (!tracker->active[key] &&
                   tracker->candidate_onset_ms[key] != 0 &&
                   timestamp_ms - tracker->candidate_onset_ms[key] >
                       PIANO_TRACKER_CANDIDATE_TIMEOUT_MS &&
                   !(reliable_interval &&
                     interval_contains_key(interval_keys, key))) {
            tracker->candidate_onset_ms[key] = 0;
            tracker->peak_evidence[key] = 0.0f;
        }

        if (tracker->active[key]) {
            tracker->peak_evidence[key] = fmaxf(
                tracker->peak_evidence[key], raw);
            const int midi = PIANO_TRACKER_MIDI_MIN + key;
            const bool low_falling = midi <= PIANO_TRACKER_LOW_TAIL_MIDI_MAX &&
                !low_yin_sustains_key(low_yin, key) &&
                raw <= previous * 1.05f &&
                raw < tracker->peak_evidence[key] *
                    PIANO_TRACKER_LOW_TAIL_PEAK_RATIO;
            if (low_falling) {
                if (tracker->falling_frames[key] < UINT8_MAX) {
                    ++tracker->falling_frames[key];
                }
            } else if (raw >= previous) {
                tracker->falling_frames[key] = 0;
            }
            if (!tracker->decay_tail[key] &&
                tracker->falling_frames[key] >=
                    PIANO_TRACKER_LOW_TAIL_FALL_FRAMES) {
                tracker->decay_tail[key] = true;
                ESP_LOGI(TAG,
                         "low tail midi=%d raw=%.2f peak=%.2f fall=%u",
                         midi, (double)raw,
                         (double)tracker->peak_evidence[key],
                         (unsigned)tracker->falling_frames[key]);
            }
        }
        tracker->previous_evidence[key] = raw;
    }
}

static uint32_t key_event_time(const piano_note_tracker_t *tracker, int key)
{
    if (key < 0 || key >= CHORD_PIANO_KEY_COUNT) return 0;
    return tracker->candidate_onset_ms[key] != 0
        ? tracker->candidate_onset_ms[key] : tracker->onset_ms[key];
}

static bool keys_share_onset(const piano_note_tracker_t *tracker,
                             int first_key, int second_key)
{
    const uint32_t first_ms = key_event_time(tracker, first_key);
    const uint32_t second_ms = key_event_time(tracker, second_key);
    if (first_ms == 0 || second_ms == 0) return false;
    const uint32_t difference = first_ms >= second_ms
        ? first_ms - second_ms : second_ms - first_ms;
    return difference <= PIANO_TRACKER_CO_ONSET_MS;
}

static bool interval_matches_pair(const chord_result_t *spectrum,
                                  int first_midi, int second_midi)
{
    int interval_keys[2];
    if (!reliable_interval_keys(spectrum, interval_keys)) return false;
    const int interval_first = PIANO_TRACKER_MIDI_MIN + interval_keys[0];
    const int interval_second = PIANO_TRACKER_MIDI_MIN + interval_keys[1];
    return (interval_first == first_midi &&
            interval_second == second_midi) ||
           (interval_first == second_midi &&
            interval_second == first_midi);
}

typedef struct {
    bool valid;
    int anchor_key;
    int secondary_key;
    int partner_key;
} secondary_pair_t;

static secondary_pair_t find_confirmed_pair(
    const piano_note_tracker_t *tracker, int dominant_key)
{
    secondary_pair_t pair = {0};
    for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
        if (!tracker->secondary_confirmed[key]) continue;
        const int anchor_key = tracker->secondary_anchor_key[key];
        if (anchor_key < 0 || anchor_key >= CHORD_PIANO_KEY_COUNT) continue;
        if (dominant_key == anchor_key) {
            pair.valid = true;
            pair.anchor_key = anchor_key;
            pair.secondary_key = key;
            pair.partner_key = key;
            return pair;
        }
        if (dominant_key == key) {
            pair.valid = true;
            pair.anchor_key = anchor_key;
            pair.secondary_key = key;
            pair.partner_key = anchor_key;
            return pair;
        }
    }
    return pair;
}

static bool has_any_confirmed_pair(const piano_note_tracker_t *tracker)
{
    for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
        if (tracker->secondary_confirmed[key]) return true;
    }
    return false;
}

static void clear_secondary_slot(piano_note_tracker_t *tracker, int key)
{
    tracker->secondary_frames[key] = 0;
    tracker->secondary_anchor_key[key] = -1;
    tracker->secondary_confirmed[key] = false;
}

static void decay_secondary_slot(piano_note_tracker_t *tracker, int key)
{
    if (tracker->secondary_frames[key] == 0) return;
    --tracker->secondary_frames[key];
    if (tracker->secondary_frames[key] != 0) return;
    if (tracker->secondary_confirmed[key]) {
        ESP_LOGI(TAG, "second note released primary=%d secondary=%d",
                 PIANO_TRACKER_MIDI_MIN +
                     tracker->secondary_anchor_key[key],
                 PIANO_TRACKER_MIDI_MIN + key);
    }
    clear_secondary_slot(tracker, key);
}

static void decay_all_secondary(piano_note_tracker_t *tracker,
                                int except_key)
{
    for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
        if (key != except_key) decay_secondary_slot(tracker, key);
    }
}

static int select_secondary_candidate(const piano_note_tracker_t *tracker,
                                      const chord_result_t *spectrum,
                                      int dominant_key,
                                      const yin_result_t *low_yin)
{
    int candidate = -1;
    for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
        if (tracker->decay_tail[key] ||
            tracker->low_rearm_blocked[key]) continue;
        if (!is_secondary_candidate(spectrum, key, dominant_key, low_yin)) {
            continue;
        }
        if (candidate < 0 ||
            spectrum->key_salience[key] >
                spectrum->key_salience[candidate]) {
            candidate = key;
        }
    }
    return candidate;
}

static secondary_pair_t update_secondary_confirmation(
    piano_note_tracker_t *tracker, const chord_result_t *spectrum,
    const yin_result_t *low_yin, bool signal_active, bool allow_attack,
    int dominant_key)
{
    /* A clipped active frame freezes confirmation history. Silence and valid
     * frames without the pair decay it, so stale pairs cannot live forever. */
    if (signal_active && !allow_attack) {
        return find_confirmed_pair(tracker, dominant_key);
    }
    if (!signal_active || dominant_key < 0 ||
        dominant_key >= CHORD_PIANO_KEY_COUNT) {
        decay_all_secondary(tracker, -1);
        return (secondary_pair_t){0};
    }

    secondary_pair_t pair = find_confirmed_pair(tracker, dominant_key);
    const int candidate = select_secondary_candidate(
        tracker, spectrum, dominant_key, low_yin);
    if (pair.valid) {
        const int storage_key = pair.secondary_key;
        const int dominant_midi = PIANO_TRACKER_MIDI_MIN + dominant_key;
        const int partner_midi = PIANO_TRACKER_MIDI_MIN + pair.partner_key;
        if (!tracker->decay_tail[pair.partner_key] &&
            is_secondary_candidate(spectrum, pair.partner_key,
                                   dominant_key, low_yin) &&
            interval_matches_pair(spectrum, dominant_midi, partner_midi)) {
            tracker->secondary_frames[storage_key] =
                PIANO_TRACKER_SECONDARY_CONFIRM_FRAMES;
        } else {
            /* The poly lock already supplies the one-frame dropout hold.
             * Do not let this legacy pair path keep an interval visible after
             * its confidence or per-note support falls below the strict gate. */
            clear_secondary_slot(tracker, storage_key);
        }
        decay_all_secondary(tracker, storage_key);
        return find_confirmed_pair(tracker, dominant_key);
    }

    /* Do not mix a transient third primary into an already confirmed pair.
     * Let the old pair decay first, then a new pair may start accumulating. */
    if (has_any_confirmed_pair(tracker)) {
        decay_all_secondary(tracker, -1);
        return (secondary_pair_t){0};
    }

    if (candidate < 0) {
        decay_all_secondary(tracker, -1);
        return (secondary_pair_t){0};
    }
    if (!keys_share_onset(tracker, dominant_key, candidate)) {
        decay_all_secondary(tracker, -1);
        return (secondary_pair_t){0};
    }

    const int dominant_midi = PIANO_TRACKER_MIDI_MIN + dominant_key;
    const int candidate_midi = PIANO_TRACKER_MIDI_MIN + candidate;
    if (!interval_matches_pair(spectrum, dominant_midi, candidate_midi)) {
        /* The former relaxed path admitted unrelated residual peaks such as
         * B2+A#6. Polyphony now requires a real interval/chord decision. */
        decay_all_secondary(tracker, -1);
        return (secondary_pair_t){0};
    }

    /* Treat the pair as unordered. If YIN swaps from F4 to C4 while the
     * candidate swaps from C4 to F4, continue the same confirmation slot. */
    int storage_key = candidate;
    if (tracker->secondary_frames[dominant_key] > 0 &&
        tracker->secondary_anchor_key[dominant_key] == candidate) {
        storage_key = dominant_key;
    }
    decay_all_secondary(tracker, storage_key);
    if (storage_key == candidate &&
        tracker->secondary_anchor_key[storage_key] != dominant_key) {
        clear_secondary_slot(tracker, storage_key);
        tracker->secondary_anchor_key[storage_key] = (int8_t)dominant_key;
    }

    const bool interval_accelerated = interval_matches_pair(
        spectrum, dominant_midi, candidate_midi);
    unsigned increment = interval_accelerated ? 2U : 1U;
    unsigned frames = tracker->secondary_frames[storage_key] + increment;
    if (frames > PIANO_TRACKER_SECONDARY_CONFIRM_FRAMES) {
        frames = PIANO_TRACKER_SECONDARY_CONFIRM_FRAMES;
    }
    tracker->secondary_frames[storage_key] = (uint8_t)frames;
    if (frames >= PIANO_TRACKER_SECONDARY_CONFIRM_FRAMES) {
        tracker->secondary_confirmed[storage_key] = true;
        ESP_LOGI(TAG,
                 "second note confirmed primary=%d secondary=%d frames=%u interval=%s",
                 dominant_midi, candidate_midi, frames,
                 interval_accelerated ? "yes" : "no");
        return find_confirmed_pair(tracker, dominant_key);
    }
    return (secondary_pair_t){0};
}

static bool belongs_to_top_four(const float evidence[CHORD_PIANO_KEY_COUNT],
                                int key)
{
    int stronger = 0;
    for (int other = 0; other < CHORD_PIANO_KEY_COUNT; ++other) {
        if (evidence[other] > evidence[key] ||
            (evidence[other] == evidence[key] && other < key)) {
            ++stronger;
        }
    }
    return stronger < PIANO_NOTE_SET_MAX_KEYS;
}

static uint8_t velocity_from_level(float rms, float salience)
{
    const float level = clamp01(rms / PIANO_TRACKER_VELOCITY_RMS);
    const float combined = sqrtf(level) * (0.72f + 0.28f * clamp01(salience));
    int velocity = (int)lrintf(127.0f * clamp01(combined));
    if (velocity < 1) velocity = 1;
    if (velocity > 127) velocity = 127;
    return (uint8_t)velocity;
}

static void clear_released_key_state(piano_note_tracker_t *tracker, int key)
{
    const bool low_key = PIANO_TRACKER_MIDI_MIN + key <=
        PIANO_TRACKER_LOW_TAIL_MIDI_MAX;
    tracker->active[key] = false;
    tracker->attack_frames[key] = 0;
    tracker->release_frames[key] = 0;
    tracker->onset_confidence[key] = 0.0f;
    tracker->onset_velocity[key] = 0;
    tracker->candidate_onset_ms[key] = 0;
    tracker->onset_ms[key] = 0;
    tracker->peak_evidence[key] = 0.0f;
    tracker->falling_frames[key] = 0;
    tracker->fresh_onset[key] = false;
    tracker->decay_tail[key] = false;
    if (low_key) {
        tracker->low_rearm_blocked[key] = true;
        tracker->low_rearm_quiet_frames[key] = 0;
    }
}

static float matched_low_confidence(const low_note_result_t *low_notes,
                                    int midi)
{
    if (low_notes == NULL || !low_notes->valid) return 0.0f;
    for (uint8_t index = 0; index < low_notes->count; ++index) {
        if (low_notes->midi[index] == midi) {
            return clamp01(low_notes->confidence[index]);
        }
    }
    return 0.0f;
}

static bool apply_locked_poly(piano_note_tracker_t *tracker,
                              const chord_result_t *spectrum,
                              const low_note_result_t *low_notes,
                              float rms, uint32_t timestamp_ms)
{
    bool changed = false;
    for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
        const int midi = PIANO_TRACKER_MIDI_MIN + key;
        bool should_be_active = false;
        for (uint8_t index = 0; index < tracker->poly_locked_count; ++index) {
            if (tracker->poly_locked_midi[index] == midi) {
                should_be_active = true;
                break;
            }
        }
        if (!should_be_active) {
            if (tracker->active[key]) {
                clear_released_key_state(tracker, key);
                changed = true;
            }
            continue;
        }

        tracker->low_rearm_blocked[key] = false;
        tracker->low_rearm_quiet_frames[key] = 0;
        tracker->decay_tail[key] = false;
        tracker->falling_frames[key] = 0;
        tracker->release_frames[key] = 0;
        tracker->attack_frames[key] = 0;
        if (!tracker->active[key]) {
            tracker->active[key] = true;
            tracker->onset_ms[key] = timestamp_ms;
            tracker->candidate_onset_ms[key] = timestamp_ms;
            tracker->onset_confidence[key] = fmaxf(
                clamp01(spectrum->confidence),
                matched_low_confidence(low_notes, midi));
            tracker->onset_velocity[key] = velocity_from_level(
                rms, spectrum->key_salience[key]);
            tracker->peak_evidence[key] = spectrum->key_salience[key];
            changed = true;
        } else {
            tracker->peak_evidence[key] = fmaxf(
                tracker->peak_evidence[key], spectrum->key_salience[key]);
        }
    }
    for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
        clear_secondary_slot(tracker, key);
    }
    tracker->single_lock_active = false;
    tracker->single_lock_midi = -1;
    return changed;
}

static bool release_decayed_low_keys(piano_note_tracker_t *tracker)
{
    bool changed = false;
    for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
        const int midi = PIANO_TRACKER_MIDI_MIN + key;
        if (!tracker->active[key] || !tracker->decay_tail[key] ||
            midi > PIANO_TRACKER_LOW_TAIL_MIDI_MAX) {
            continue;
        }
        ESP_LOGI(TAG, "low tail release midi=%d", midi);
        clear_released_key_state(tracker, key);
        tracker->decay_tail[key] = true;
        changed = true;
    }
    return changed;
}

static bool release_low_tails_for_new_onset(piano_note_tracker_t *tracker,
                                            const chord_result_t *spectrum,
                                            int dominant_key,
                                            uint32_t timestamp_ms)
{
    bool changed = false;
    int interval_keys[2] = {-1, -1};
    const bool reliable_interval = reliable_interval_keys(
        spectrum, interval_keys);
    for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
        const int midi = PIANO_TRACKER_MIDI_MIN + key;
        const bool below_tail_level = tracker->peak_evidence[key] > 0.0f &&
            tracker->previous_evidence[key] <
                tracker->peak_evidence[key] *
                    PIANO_TRACKER_LOW_TAIL_PEAK_RATIO;
        const bool older_event = tracker->onset_ms[key] != 0 &&
            timestamp_ms - tracker->onset_ms[key] >
                PIANO_TRACKER_CO_ONSET_MS &&
            !tracker->fresh_onset[key];
        if (!tracker->active[key] || midi > PIANO_TRACKER_LOW_TAIL_MIDI_MAX ||
            (!tracker->decay_tail[key] && !below_tail_level &&
             !older_event)) {
            continue;
        }
        int new_key = -1;
        for (int other = 0; other < CHORD_PIANO_KEY_COUNT; ++other) {
            const bool trusted_new_onset = tracker->fresh_onset[other] &&
                (other == dominant_key ||
                 spectrum->key_salience[other] >=
                     PIANO_TRACKER_ATTACK_THRESHOLD ||
                 (reliable_interval &&
                  interval_contains_key(interval_keys, other)));
            if (other != key && trusted_new_onset) {
                new_key = other;
                break;
            }
        }
        if (new_key < 0) continue;
        ESP_LOGI(TAG,
                 "low tail fast release midi=%d next=%d age=%" PRIu32 "ms",
                 midi, PIANO_TRACKER_MIDI_MIN + new_key,
                 tracker->onset_ms[key] != 0
                     ? timestamp_ms - tracker->onset_ms[key] : 0);
        clear_released_key_state(tracker, key);
        /* Keep the acoustic residue suppressed until this same key produces
         * a genuine new rise; otherwise it could re-enter later in this frame. */
        tracker->decay_tail[key] = true;
        changed = true;
    }
    return changed;
}

static void make_snapshot(const piano_note_tracker_t *tracker,
                          uint32_t timestamp_ms, bool overflow,
                          piano_note_set_t *out_set)
{
    memset(out_set, 0, sizeof(*out_set));
    out_set->timestamp_ms = timestamp_ms;
    out_set->overflow = overflow;
    out_set->degraded_mic = tracker->degraded_mic;
    for (int key = 0; key < CHORD_PIANO_KEY_COUNT &&
                      out_set->count < PIANO_NOTE_SET_MAX_KEYS; ++key) {
        if (!tracker->active[key]) continue;
        const uint8_t index = out_set->count++;
        out_set->midi[index] = PIANO_TRACKER_MIDI_MIN + key;
        out_set->confidence[index] = tracker->onset_confidence[key];
        out_set->velocity[index] = tracker->onset_velocity[key];
    }
    for (uint8_t index = out_set->count;
         index < PIANO_NOTE_SET_MAX_KEYS; ++index) {
        out_set->midi[index] = -1;
    }
}

void piano_note_tracker_init(piano_note_tracker_t *tracker)
{
    if (tracker == NULL) return;
    memset(tracker, 0, sizeof(*tracker));
    for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
        tracker->secondary_anchor_key[key] = -1;
    }
    for (uint8_t index = 0; index < PIANO_NOTE_SET_MAX_KEYS; ++index) {
        tracker->poly_candidate_midi[index] = -1;
        tracker->poly_locked_midi[index] = -1;
    }
    tracker->single_lock_midi = -1;
    tracker->interval_alignment_from_midi = -1;
    tracker->interval_alignment_to_midi = -1;
}

bool piano_note_tracker_update(piano_note_tracker_t *tracker,
                               const chord_result_t *spectrum,
                               const yin_result_t *high_yin,
                               const yin_result_t *low_yin,
                               const low_note_result_t *low_notes,
                               float harmonic_explained_ratio,
                               bool signal_active, bool allow_attack,
                               bool degraded_mic, float rms,
                               uint32_t timestamp_ms,
                               piano_note_set_t *out_set)
{
    if (tracker == NULL || spectrum == NULL || out_set == NULL) return false;
    float *evidence = tracker->evidence;
    memcpy(evidence, spectrum->key_salience, sizeof(tracker->evidence));
    if (signal_active && allow_attack) {
        for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
            if (!spectrum->key_uses_virtual_fundamental[key]) continue;
            if (!virtual_low_confirmed(spectrum, key, low_yin) &&
                !strong_virtual_low(spectrum, key)) {
                evidence[key] = 0.0f;
            }
        }
        add_yin_evidence(evidence, high_yin,
                         PIANO_TRACKER_HIGH_YIN_CONFIDENCE);
        if (low_yin != NULL && low_yin->valid &&
            low_yin->midi >= PIANO_TRACKER_MIDI_MIN &&
            low_yin->midi <= PIANO_TRACKER_LOW_TAIL_MIDI_MAX) {
            const int low_key = low_yin->midi - PIANO_TRACKER_MIDI_MIN;
            if (has_fundamental_support(spectrum, low_key, low_yin)) {
                add_yin_evidence(evidence, low_yin,
                                 PIANO_TRACKER_LOW_YIN_CONFIDENCE);
            }
        }

        const bool low_poly_anchor = low_poly_has_periodicity_anchor(
            spectrum, low_yin, low_notes);
        for (int key = 0;
             key < MUSIC_FFT_POLY_PRIORITY_MIN_MIDI -
                       PIANO_TRACKER_MIDI_MIN;
             ++key) {
            const int midi = PIANO_TRACKER_MIDI_MIN + key;
            const bool direct_periodicity = low_periodicity_supports_midi(
                low_yin, low_notes, midi);
            const bool physical_chord_companion = low_poly_anchor &&
                spectrum_contains_exact_midi(spectrum, midi) &&
                spectrum->key_has_independent_fundamental[key];
            const bool physical_sustain = tracker->active[key] &&
                spectrum->key_has_independent_fundamental[key];
            if (!direct_periodicity && !physical_chord_companion &&
                !physical_sustain) {
                evidence[key] = 0.0f;
            }
        }
    } else {
        memset(evidence, 0, sizeof(tracker->evidence));
    }

    bool changed = !tracker->degraded_initialized ||
                   tracker->degraded_mic != degraded_mic;
    tracker->degraded_initialized = true;
    tracker->degraded_mic = degraded_mic;

    const bool clipped_active = signal_active && !allow_attack;
    if (!clipped_active) {
        tracker->last_clean_ms = timestamp_ms;
    } else if (tracker->last_clean_ms != 0 &&
               timestamp_ms - tracker->last_clean_ms <=
                   PIANO_TRACKER_CLIP_HOLD_MS) {
        /* Do not turn an already confirmed note set into an empty screen
         * during the short interval in which adaptive gain is stepping down. */
        make_snapshot(tracker, timestamp_ms, false, out_set);
        return changed;
    }
    const bool analysis_signal_active = clipped_active ? false : signal_active;
    const bool analysis_allow_attack = analysis_signal_active && allow_attack;
    update_key_event_envelopes(tracker, spectrum, low_yin,
                               analysis_signal_active,
                               analysis_signal_active ? rms : 0.0f,
                               timestamp_ms);

    const bool poly_was_locked = tracker->poly_lock_active;
    if (update_poly_lock(tracker, spectrum, low_yin, low_notes,
                         analysis_signal_active, analysis_allow_attack)) {
        if (apply_locked_poly(tracker, spectrum, low_notes, rms,
                              timestamp_ms)) {
            changed = true;
        }
        make_snapshot(tracker, timestamp_ms, false, out_set);
        return changed;
    }
    if (poly_was_locked && !tracker->poly_lock_active) {
        for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
            if (!tracker->active[key]) continue;
            clear_released_key_state(tracker, key);
            changed = true;
        }
    }
    if (release_decayed_low_keys(tracker)) changed = true;

    const yin_result_t *dominant_yin = select_dominant_yin(
        spectrum, high_yin, low_yin);
    const bool dominant_in_range = dominant_yin != NULL &&
        dominant_yin->midi >= PIANO_TRACKER_MIDI_MIN &&
        dominant_yin->midi <= PIANO_TRACKER_MIDI_MAX;
    const int original_dominant_key = dominant_in_range
        ? dominant_yin->midi - PIANO_TRACKER_MIDI_MIN : -1;
    const int dominant_key = dominant_in_range
        ? align_dominant_key_to_interval(
              spectrum, dominant_yin, original_dominant_key)
        : -1;
    const bool interval_aligned = dominant_in_range &&
        dominant_key != original_dominant_key;
    if (interval_aligned) {
        const int from_midi = PIANO_TRACKER_MIDI_MIN + original_dominant_key;
        const int to_midi = PIANO_TRACKER_MIDI_MIN + dominant_key;
        if (!tracker->interval_alignment_active ||
            tracker->interval_alignment_from_midi != from_midi ||
            tracker->interval_alignment_to_midi != to_midi) {
            ESP_LOGI(TAG,
                     "interval align yin=%d -> midi=%d freq=%.1fHz conf=%.2f",
                     from_midi, to_midi,
                     (double)dominant_yin->frequency_hz,
                     (double)spectrum->confidence);
        }
        tracker->interval_alignment_active = true;
        tracker->interval_alignment_from_midi = from_midi;
        tracker->interval_alignment_to_midi = to_midi;
        const float supported = 0.55f +
            0.45f * clamp01(dominant_yin->confidence);
        evidence[dominant_key] = fmaxf(evidence[dominant_key], supported);
        /* The accepted physical interval owns this boundary decision. Do not
         * leave the quantized YIN neighbour available as a third exact key. */
        evidence[original_dominant_key] = 0.0f;
    } else {
        tracker->interval_alignment_active = false;
        tracker->interval_alignment_from_midi = -1;
        tracker->interval_alignment_to_midi = -1;
    }
    if (release_low_tails_for_new_onset(
            tracker, spectrum, dominant_key, timestamp_ms)) {
        changed = true;
    }
    for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
        if ((tracker->decay_tail[key] ||
             tracker->low_rearm_blocked[key]) &&
            !tracker->fresh_onset[key]) {
            evidence[key] = 0.0f;
        }
    }
    unsigned independent_count = 0;
    for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
        if (tracker->decay_tail[key] ||
            tracker->low_rearm_blocked[key] ||
            !has_trusted_independent_fundamental(spectrum, key, low_yin)) {
            continue;
        }
        ++independent_count;
    }
    const int virtual_low_key = strongest_virtual_low_key(
        tracker, spectrum, low_yin);
    const bool dominant_available = dominant_in_range &&
        !tracker->low_rearm_blocked[dominant_key] &&
        (!tracker->decay_tail[dominant_key] ||
         tracker->fresh_onset[dominant_key]);
    if (dominant_in_range && !dominant_available) {
        evidence[dominant_key] = 0.0f;
    }
    const bool dominant_confident = dominant_available &&
        single_candidate_confident(
            spectrum, dominant_yin, dominant_key, low_yin,
            harmonic_explained_ratio);
    const secondary_pair_t secondary_pair = update_secondary_confirmation(
        tracker, spectrum, low_yin, analysis_signal_active,
        analysis_allow_attack,
        dominant_available ? dominant_key : -1);
    const bool single_lock = analysis_signal_active &&
        analysis_allow_attack &&
        dominant_confident &&
        dominant_yin->confidence >=
            PIANO_TRACKER_SINGLE_LOCK_YIN_CONFIDENCE &&
        harmonic_explained_ratio >=
            PIANO_TRACKER_SINGLE_LOCK_HARMONIC_RATIO &&
        independent_count < 2 && !secondary_pair.valid;

    if (single_lock) {
        for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
            if (key == dominant_key) continue;
            evidence[key] = 0.0f;
            tracker->attack_frames[key] = 0;
            tracker->release_frames[key] = 0;
            if (tracker->active[key]) {
                const bool preserve_tail = tracker->decay_tail[key];
                clear_released_key_state(tracker, key);
                tracker->decay_tail[key] = preserve_tail;
                changed = true;
            }
        }
    } else if (secondary_pair.valid) {
        /* A weaker physical second note may unlock only this confirmed pair;
         * it must not globally relax the third/fourth-note admission gate. */
        for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
            if (key != dominant_key &&
                key != secondary_pair.partner_key) {
                evidence[key] = 0.0f;
            }
        }
        if (analysis_signal_active && analysis_allow_attack &&
            !tracker->decay_tail[secondary_pair.partner_key] &&
            !tracker->low_rearm_blocked[secondary_pair.partner_key] &&
            is_secondary_candidate(spectrum,
                                   secondary_pair.partner_key,
                                   dominant_key, low_yin)) {
            evidence[secondary_pair.partner_key] = fmaxf(
                evidence[secondary_pair.partner_key],
                PIANO_TRACKER_ATTACK_THRESHOLD);
        }
    } else {
        /* Outside a confirmed poly lock, admit only one strictly qualified
         * pitch. Unqualified spectral peaks must not reach the exact stream. */
        int mono_key = -1;
        if (dominant_confident &&
            evidence[dominant_key] >= PIANO_TRACKER_ATTACK_THRESHOLD) {
            mono_key = dominant_key;
        } else if (virtual_low_key >= 0) {
            mono_key = virtual_low_key;
            evidence[mono_key] = fmaxf(
                evidence[mono_key], PIANO_TRACKER_ATTACK_THRESHOLD);
        }
        for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
            if (key != mono_key) evidence[key] = 0.0f;
        }
    }

    if (single_lock &&
        (!tracker->single_lock_active ||
         tracker->single_lock_midi !=
             PIANO_TRACKER_MIDI_MIN + dominant_key)) {
        ESP_LOGI(TAG,
                 "single lock midi=%d yin=%.2f harmonic=%.2f independent=%u pair=%s",
                 PIANO_TRACKER_MIDI_MIN + dominant_key,
                 (double)dominant_yin->confidence,
                 (double)harmonic_explained_ratio, independent_count,
                 secondary_pair.valid ? "yes" : "no");
    } else if (!single_lock && tracker->single_lock_active) {
        ESP_LOGI(TAG, "single lock released independent=%u pair=%s",
                 independent_count,
                 secondary_pair.valid ? "yes" : "no");
    }
    tracker->single_lock_active = single_lock;
    tracker->single_lock_midi = single_lock
        ? PIANO_TRACKER_MIDI_MIN + dominant_key : -1;

    unsigned supported_count = 0;
    for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
        if (evidence[key] >= PIANO_TRACKER_RELEASE_THRESHOLD) {
            ++supported_count;
        }
    }
    const bool overflow = analysis_signal_active &&
                          supported_count > PIANO_NOTE_SET_MAX_KEYS;
    if (overflow) {
        /* More than four supported keys is outside the declared model. Keep
         * the last trusted set instead of presenting the loudest four as an
         * exact answer. */
        make_snapshot(tracker, timestamp_ms, true, out_set);
        return changed;
    }
    for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
        const bool top_four = belongs_to_top_four(evidence, key);
        const bool attack_supported = analysis_signal_active &&
            analysis_allow_attack &&
            top_four &&
            evidence[key] >= PIANO_TRACKER_ATTACK_THRESHOLD;
        const bool tail_below_release = tracker->decay_tail[key] &&
            tracker->peak_evidence[key] > 0.0f &&
            spectrum->key_salience[key] <
                tracker->peak_evidence[key] *
                    PIANO_TRACKER_LOW_TAIL_RELEASE_RATIO;
        const bool release_supported = analysis_signal_active && top_four &&
            evidence[key] >= PIANO_TRACKER_RELEASE_THRESHOLD &&
            !tail_below_release;
        if (tracker->active[key]) {
            tracker->attack_frames[key] = 0;
            if (release_supported) {
                tracker->release_frames[key] = 0;
            } else if (++tracker->release_frames[key] >=
                       PIANO_TRACKER_RELEASE_FRAMES) {
                const bool preserve_tail = tracker->decay_tail[key];
                clear_released_key_state(tracker, key);
                tracker->decay_tail[key] = preserve_tail;
                changed = true;
            }
        } else {
            tracker->release_frames[key] = 0;
            if (attack_supported) {
                if (++tracker->attack_frames[key] >=
                    PIANO_TRACKER_ATTACK_FRAMES) {
                    tracker->active[key] = true;
                    tracker->attack_frames[key] = 0;
                    tracker->onset_confidence[key] = clamp01(evidence[key]);
                    tracker->onset_velocity[key] = velocity_from_level(
                        rms, evidence[key]);
                    tracker->onset_ms[key] =
                        tracker->candidate_onset_ms[key] != 0
                            ? tracker->candidate_onset_ms[key]
                            : timestamp_ms;
                    tracker->peak_evidence[key] = fmaxf(
                        tracker->peak_evidence[key],
                        spectrum->key_salience[key]);
                    tracker->falling_frames[key] = 0;
                    tracker->decay_tail[key] = false;
                    changed = true;
                }
            } else {
                tracker->attack_frames[key] = 0;
            }
        }
    }

    make_snapshot(tracker, timestamp_ms, overflow, out_set);
    return changed;
}

bool piano_note_tracker_release_all(piano_note_tracker_t *tracker,
                                    uint32_t timestamp_ms,
                                    piano_note_set_t *out_set)
{
    if (tracker == NULL || out_set == NULL) return false;
    bool changed = false;
    for (int key = 0; key < CHORD_PIANO_KEY_COUNT; ++key) {
        changed = changed || tracker->active[key];
    }
    piano_note_tracker_init(tracker);
    make_snapshot(tracker, timestamp_ms, false, out_set);
    return changed;
}
