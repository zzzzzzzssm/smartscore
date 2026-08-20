#include "low_note_detector.h"

#include <math.h>
#include <string.h>

#include "note_utils.h"

static float clamp01(float value)
{
    return fmaxf(0.0f, fminf(1.0f, value));
}

static void initialize_fir(low_note_detector_t *detector)
{
    const int center = ((int)MUSIC_LOW_MATCH_FIR_TAPS - 1) / 2;
    const float normalized_cutoff = MUSIC_LOW_MATCH_CUTOFF_HZ /
                                    MUSIC_SAMPLE_RATE_HZ;
    float sum = 0.0f;
    for (int tap = 0; tap < (int)MUSIC_LOW_MATCH_FIR_TAPS; ++tap) {
        const int offset = tap - center;
        const float ideal = offset == 0
            ? 2.0f * normalized_cutoff
            : sinf(2.0f * (float)M_PI * normalized_cutoff * offset) /
                  ((float)M_PI * offset);
        const float window = 0.54f - 0.46f * cosf(
            2.0f * (float)M_PI * tap /
            ((float)MUSIC_LOW_MATCH_FIR_TAPS - 1.0f));
        detector->fir[tap] = ideal * window;
        sum += detector->fir[tap];
    }
    if (fabsf(sum) > 1.0e-9f) {
        for (size_t tap = 0; tap < MUSIC_LOW_MATCH_FIR_TAPS; ++tap) {
            detector->fir[tap] /= sum;
        }
    }
}

static void initialize_analysis_tables(low_note_detector_t *detector)
{
    for (size_t index = 0; index < MUSIC_LOW_MATCH_WINDOW_SIZE; ++index) {
        detector->hann[index] = 0.5f - 0.5f * cosf(
            2.0f * (float)M_PI * index /
            ((float)MUSIC_LOW_MATCH_WINDOW_SIZE - 1.0f));
    }
    for (int key = 0; key < LOW_NOTE_DETECTOR_KEY_COUNT; ++key) {
        const float f0 = note_midi_to_frequency(
            LOW_NOTE_DETECTOR_MIDI_MIN + key);
        for (size_t harmonic = 0;
             harmonic < MUSIC_LOW_MATCH_MAX_HARMONICS; ++harmonic) {
            const float frequency = f0 * (float)(harmonic + 1U);
            const bool valid = frequency <= MUSIC_LOW_MATCH_MAX_HARMONIC_HZ &&
                               frequency < MUSIC_LOW_YIN_SAMPLE_RATE_HZ * 0.5f;
            detector->harmonic_valid[key][harmonic] = valid;
            detector->goertzel_coefficient[key][harmonic] = valid
                ? 2.0f * cosf(2.0f * (float)M_PI * frequency /
                              MUSIC_LOW_YIN_SAMPLE_RATE_HZ)
                : 0.0f;
        }
    }
}

void low_note_detector_init(low_note_detector_t *detector)
{
    if (detector == NULL) return;
    memset(detector, 0, sizeof(*detector));
    initialize_fir(detector);
    initialize_analysis_tables(detector);
}

void low_note_detector_reset(low_note_detector_t *detector)
{
    if (detector == NULL) return;
    memset(detector->ring, 0, sizeof(detector->ring));
    memset(detector->ring_write, 0, sizeof(detector->ring_write));
    memset(detector->ring_filled, 0, sizeof(detector->ring_filled));
    memset(detector->fir_history, 0, sizeof(detector->fir_history));
    memset(detector->fir_write, 0, sizeof(detector->fir_write));
    memset(detector->decimation_phase, 0,
           sizeof(detector->decimation_phase));
    memset(detector->smoothed_confidence, 0,
           sizeof(detector->smoothed_confidence));
    memset(detector->attack_frames, 0, sizeof(detector->attack_frames));
    memset(detector->release_frames, 0, sizeof(detector->release_frames));
    memset(detector->active, 0, sizeof(detector->active));
}

void low_note_detector_push(low_note_detector_t *detector, int mic_index,
                            const float *samples, size_t count)
{
    if (detector == NULL || samples == NULL || mic_index < 0 ||
        mic_index >= 2) {
        return;
    }
    for (size_t sample = 0; sample < count; ++sample) {
        detector->fir_history[mic_index]
            [detector->fir_write[mic_index]] = samples[sample];
        detector->fir_write[mic_index] =
            (detector->fir_write[mic_index] + 1U) %
            MUSIC_LOW_MATCH_FIR_TAPS;
        detector->decimation_phase[mic_index] =
            (uint8_t)((detector->decimation_phase[mic_index] + 1U) %
                      MUSIC_LOW_YIN_DECIMATION);
        if (detector->decimation_phase[mic_index] != 0U) continue;

        float filtered = 0.0f;
        size_t history = detector->fir_write[mic_index];
        for (size_t tap = 0; tap < MUSIC_LOW_MATCH_FIR_TAPS; ++tap) {
            history = history == 0U
                ? MUSIC_LOW_MATCH_FIR_TAPS - 1U : history - 1U;
            filtered += detector->fir[tap] *
                        detector->fir_history[mic_index][history];
        }
        const size_t write = detector->ring_write[mic_index];
        detector->ring[mic_index][write] = filtered;
        detector->ring_write[mic_index] =
            (write + 1U) & (MUSIC_LOW_MATCH_RING_SIZE - 1U);
        if (detector->ring_filled[mic_index] < MUSIC_LOW_MATCH_RING_SIZE) {
            ++detector->ring_filled[mic_index];
        }
    }
}

bool low_note_detector_copy_recent(const low_note_detector_t *detector,
                                   int mic_index, float *samples,
                                   size_t count)
{
    if (detector == NULL || samples == NULL || mic_index < 0 ||
        mic_index >= 2 || count > MUSIC_LOW_MATCH_RING_SIZE ||
        detector->ring_filled[mic_index] < count) {
        return false;
    }
    const size_t start = (detector->ring_write[mic_index] +
                          MUSIC_LOW_MATCH_RING_SIZE - count) &
                         (MUSIC_LOW_MATCH_RING_SIZE - 1U);
    for (size_t index = 0; index < count; ++index) {
        samples[index] = detector->ring[mic_index]
            [(start + index) & (MUSIC_LOW_MATCH_RING_SIZE - 1U)];
    }
    return true;
}

static float goertzel_magnitude(const float *samples, size_t count,
                                float coefficient)
{
    float previous = 0.0f;
    float previous2 = 0.0f;
    for (size_t index = 0; index < count; ++index) {
        const float current = samples[index] + coefficient * previous -
                              previous2;
        previous2 = previous;
        previous = current;
    }
    const float power = fmaxf(0.0f, previous * previous +
        previous2 * previous2 - coefficient * previous * previous2);
    return 4.0f * sqrtf(power) / (float)count;
}

static float robust_floor(float values[LOW_NOTE_DETECTOR_KEY_COUNT],
                          size_t count)
{
    for (size_t index = 1; index < count; ++index) {
        const float value = values[index];
        size_t move = index;
        while (move > 0U && values[move - 1U] > value) {
            values[move] = values[move - 1U];
            --move;
        }
        values[move] = value;
    }
    return count > 0U ? values[count / 3U] : 0.0f;
}

static float score_key(const low_note_detector_t *detector, int key,
                       const float amplitudes[LOW_NOTE_DETECTOR_KEY_COUNT]
                                             [MUSIC_LOW_MATCH_MAX_HARMONICS])
{
    static const float weights[MUSIC_LOW_MATCH_MAX_HARMONICS] = {
        0.80f, 1.00f, 0.95f, 0.70f, 0.55f, 0.45f, 0.35f, 0.30f,
    };
    float weighted = 0.0f;
    float total_weight = 0.0f;
    float fundamental = 0.0f;
    float second = 0.0f;
    float third = 0.0f;
    unsigned coherent = 0;
    for (size_t harmonic = 0;
         harmonic < MUSIC_LOW_MATCH_MAX_HARMONICS; ++harmonic) {
        if (!detector->harmonic_valid[key][harmonic]) continue;
        const float noise = fmaxf(detector->noise_floor[harmonic],
                                  MUSIC_LOW_MATCH_MIN_AMPLITUDE);
        const float snr = amplitudes[key][harmonic] / noise;
        const float evidence = clamp01((snr - 1.35f) / 4.65f);
        weighted += weights[harmonic] * evidence;
        total_weight += weights[harmonic];
        if (evidence >= 0.30f) ++coherent;
        if (harmonic == 0U) fundamental = evidence;
        if (harmonic == 1U) second = evidence;
        if (harmonic == 2U) third = evidence;
    }
    if (total_weight <= 0.0f ||
        !((second >= 0.25f && third >= 0.20f) || fundamental >= 0.68f)) {
        return 0.0f;
    }
    const float coherence = clamp01((float)coherent / 4.0f);
    return clamp01((weighted / total_weight) *
                   (0.42f + 0.58f * coherence));
}

static float score_to_confidence(float score)
{
    return clamp01((score - 0.16f) / 0.54f);
}

static void subtract_selected_harmonics(low_note_detector_t *detector,
                                        int selected_key)
{
    const float selected_f0 = note_midi_to_frequency(
        LOW_NOTE_DETECTOR_MIDI_MIN + selected_key);
    const float resolution = MUSIC_LOW_YIN_SAMPLE_RATE_HZ /
                             MUSIC_LOW_MATCH_WINDOW_SIZE;
    for (int key = 0; key < LOW_NOTE_DETECTOR_KEY_COUNT; ++key) {
        if (key == selected_key) continue;
        const float candidate_f0 = note_midi_to_frequency(
            LOW_NOTE_DETECTOR_MIDI_MIN + key);
        for (size_t harmonic = 0;
             harmonic < MUSIC_LOW_MATCH_MAX_HARMONICS; ++harmonic) {
            if (!detector->harmonic_valid[key][harmonic]) continue;
            const float frequency = candidate_f0 * (float)(harmonic + 1U);
            float predicted = 0.0f;
            for (size_t owner_harmonic = 0;
                 owner_harmonic < MUSIC_LOW_MATCH_MAX_HARMONICS;
                 ++owner_harmonic) {
                if (!detector->harmonic_valid[selected_key][owner_harmonic]) {
                    continue;
                }
                const float owner_frequency = selected_f0 *
                    (float)(owner_harmonic + 1U);
                const float distance = fabsf(frequency - owner_frequency);
                if (distance > 1.75f * resolution) continue;
                const float leakage = expf(-0.5f *
                    (distance / fmaxf(resolution, 1.0e-6f)) *
                    (distance / fmaxf(resolution, 1.0e-6f)));
                predicted = fmaxf(predicted,
                    detector->amplitude[selected_key][owner_harmonic] *
                    leakage * MUSIC_LOW_MATCH_LEAKAGE_SUBTRACT);
            }
            detector->residual[key][harmonic] = fmaxf(
                0.0f, detector->residual[key][harmonic] - predicted);
        }
    }
}

static bool contains_key(const int keys[MUSIC_LOW_MATCH_MAX_KEYS],
                         uint8_t count, int key)
{
    for (uint8_t index = 0; index < count; ++index) {
        if (keys[index] == key) return true;
    }
    return false;
}

void low_note_detector_analyze(low_note_detector_t *detector, int mic_index,
                               bool signal_active, bool allow_attack,
                               bool input_clipped,
                               low_note_result_t *result)
{
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));
    result->strongest_midi = -1;
    for (size_t index = 0; index < MUSIC_LOW_MATCH_MAX_KEYS; ++index) {
        result->midi[index] = -1;
    }
    if (detector == NULL || mic_index < 0 || mic_index >= 2 ||
        detector->ring_filled[mic_index] < MUSIC_LOW_MATCH_WINDOW_SIZE) {
        return;
    }

    const size_t start = (detector->ring_write[mic_index] +
                          MUSIC_LOW_MATCH_RING_SIZE -
                          MUSIC_LOW_MATCH_WINDOW_SIZE) &
                         (MUSIC_LOW_MATCH_RING_SIZE - 1U);
    for (size_t index = 0; index < MUSIC_LOW_MATCH_WINDOW_SIZE; ++index) {
        detector->window[index] = detector->ring[mic_index]
            [(start + index) & (MUSIC_LOW_MATCH_RING_SIZE - 1U)] *
            detector->hann[index];
    }

    for (int key = 0; key < LOW_NOTE_DETECTOR_KEY_COUNT; ++key) {
        for (size_t harmonic = 0;
             harmonic < MUSIC_LOW_MATCH_MAX_HARMONICS; ++harmonic) {
            const float amplitude = detector->harmonic_valid[key][harmonic]
                ? goertzel_magnitude(
                      detector->window, MUSIC_LOW_MATCH_WINDOW_SIZE,
                      detector->goertzel_coefficient[key][harmonic])
                : 0.0f;
            detector->amplitude[key][harmonic] = amplitude;
            detector->residual[key][harmonic] = amplitude;
        }
    }
    for (size_t harmonic = 0;
         harmonic < MUSIC_LOW_MATCH_MAX_HARMONICS; ++harmonic) {
        float values[LOW_NOTE_DETECTOR_KEY_COUNT];
        size_t count = 0;
        for (int key = 0; key < LOW_NOTE_DETECTOR_KEY_COUNT; ++key) {
            if (detector->harmonic_valid[key][harmonic]) {
                values[count++] = detector->amplitude[key][harmonic];
            }
        }
        detector->noise_floor[harmonic] = fmaxf(
            MUSIC_LOW_MATCH_MIN_AMPLITUDE, robust_floor(values, count));
    }

    int raw_keys[MUSIC_LOW_MATCH_MAX_KEYS];
    float raw_confidence[MUSIC_LOW_MATCH_MAX_KEYS] = {0};
    uint8_t raw_count = 0;
    float primary_confidence = 0.0f;
    while (raw_count < MUSIC_LOW_MATCH_MAX_KEYS) {
        int best_key = -1;
        float best_score = 0.0f;
        float best_confidence = 0.0f;
        for (int key = 0; key < LOW_NOTE_DETECTOR_KEY_COUNT; ++key) {
            if (contains_key(raw_keys, raw_count, key)) continue;
            const float score = score_key(detector, key, detector->residual);
            const float confidence = score_to_confidence(score);
            if (confidence > best_confidence) {
                best_key = key;
                best_score = score;
                best_confidence = confidence;
            }
        }
        if (raw_count == 0U) {
            result->strongest_midi = best_key >= 0
                ? LOW_NOTE_DETECTOR_MIDI_MIN + best_key : -1;
            result->strongest_confidence = best_confidence;
            result->strongest_score = best_score;
            primary_confidence = best_confidence;
        }
        const float threshold = raw_count == 0U
            ? MUSIC_LOW_MATCH_PRIMARY_CONFIDENCE
            : MUSIC_LOW_MATCH_SECONDARY_CONFIDENCE;
        if (best_key < 0 || best_confidence < threshold ||
            (raw_count > 0U && best_confidence <
                primary_confidence * MUSIC_LOW_MATCH_SECONDARY_RATIO)) {
            break;
        }
        raw_keys[raw_count] = best_key;
        raw_confidence[raw_count] = best_confidence;
        ++raw_count;
        subtract_selected_harmonics(detector, best_key);
    }

    const bool strong_tonal_override = !input_clipped && raw_count > 0U &&
        raw_confidence[0] >= MUSIC_LOW_MATCH_STRONG_CONFIDENCE;
    const bool effective_signal_active = signal_active ||
                                         strong_tonal_override;
    const bool effective_allow_attack = allow_attack ||
                                        strong_tonal_override;
    result->tonal_override = strong_tonal_override;

    for (int key = 0; key < LOW_NOTE_DETECTOR_KEY_COUNT; ++key) {
        float observed = 0.0f;
        for (uint8_t index = 0; index < raw_count; ++index) {
            if (raw_keys[index] == key) observed = raw_confidence[index];
        }
        if (observed > 0.0f && effective_signal_active) {
            detector->smoothed_confidence[key] =
                detector->smoothed_confidence[key] <= 0.0f
                    ? observed
                    : 0.55f * detector->smoothed_confidence[key] +
                      0.45f * observed;
            detector->release_frames[key] = 0;
            if (effective_allow_attack && !detector->active[key]) {
                if (detector->attack_frames[key] < UINT8_MAX) {
                    ++detector->attack_frames[key];
                }
                if (observed >= MUSIC_LOW_MATCH_STRONG_CONFIDENCE ||
                    detector->attack_frames[key] >=
                        MUSIC_LOW_MATCH_ATTACK_FRAMES) {
                    detector->active[key] = true;
                }
            }
        } else {
            detector->attack_frames[key] = 0;
            detector->smoothed_confidence[key] *= 0.65f;
            if (detector->active[key]) {
                if (detector->release_frames[key] < UINT8_MAX) {
                    ++detector->release_frames[key];
                }
                if (detector->release_frames[key] >=
                    MUSIC_LOW_MATCH_RELEASE_FRAMES) {
                    detector->active[key] = false;
                    detector->release_frames[key] = 0;
                }
            }
        }
        result->key_confidence[key] =
            detector->active[key] ? detector->smoothed_confidence[key] : 0.0f;
    }

    while (result->count < MUSIC_LOW_MATCH_MAX_KEYS) {
        int best_key = -1;
        float best = 0.0f;
        for (int key = 0; key < LOW_NOTE_DETECTOR_KEY_COUNT; ++key) {
            if (!detector->active[key] ||
                contains_key(result->midi, result->count,
                             LOW_NOTE_DETECTOR_MIDI_MIN + key)) {
                continue;
            }
            if (detector->smoothed_confidence[key] > best) {
                best = detector->smoothed_confidence[key];
                best_key = key;
            }
        }
        if (best_key < 0) break;
        const uint8_t output = result->count++;
        result->midi[output] = LOW_NOTE_DETECTOR_MIDI_MIN + best_key;
        result->frequency_hz[output] = note_midi_to_frequency(
            result->midi[output]);
        result->confidence[output] = best;
    }
    result->valid = result->count > 0U;
}
