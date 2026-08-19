#include "low_frequency_analyzer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chord_detector.h"
#include "esp_timer.h"
#include "music_detector_config.h"
#include "note_utils.h"

#define LOW_FFT_BIN_COUNT (MUSIC_LOW_FFT_SIZE / 2 + 1)
#define LOW_NOTE_COUNT (MUSIC_LOW_MIDI_MAX - MUSIC_LOW_MIDI_MIN + 1)
#define LOW_NOISE_HISTOGRAM_BINS 40
#define LOW_DECIMATOR_TAPS 7

typedef struct {
    low_frequency_candidate_t candidate;
    float harmonic_peak[MUSIC_LOW_TEMPLATE_HARMONICS];
    bool independent_fundamental;
} template_candidate_t;

static const float s_decimator_coefficients[LOW_DECIMATOR_TAPS] = {
    -0.0456359f, 0.0f, 0.2838293f, 0.5236132f,
     0.2838293f, 0.0f, -0.0456359f,
};

static float s_decimator_history[LOW_DECIMATOR_TAPS];
static size_t s_decimator_position;
static unsigned s_decimator_phase;
static float s_low_ring[MUSIC_LOW_FFT_SIZE];
static size_t s_low_write_position;
static size_t s_low_filled;
static float s_low_yin_window[MUSIC_LOW_YIN_WINDOW_SIZE];
static float s_low_magnitude[LOW_FFT_BIN_COUNT];
static template_candidate_t s_template_candidates[LOW_NOTE_COUNT];
static template_candidate_t
    s_active_candidates[MUSIC_LOW_TEMPLATE_CANDIDATE_COUNT];
static int s_selected_mic;
static int s_previous_single_midi = -1;
static float s_previous_single_frequency;
static int s_single_consecutive;
static int s_octave_candidate_midi = -1;
static int s_octave_consecutive;
static int s_previous_poly_identity = -1;
static int s_poly_consecutive;
static bool s_template_spectrum_seen;

static float clamp01(float value)
{
    return fmaxf(0.0f, fminf(1.0f, value));
}

static void reset_poly_history(void)
{
    s_previous_poly_identity = -1;
    s_poly_consecutive = 0;
}

static void reset_decision_history(void)
{
    s_previous_single_midi = -1;
    s_previous_single_frequency = 0.0f;
    s_single_consecutive = 0;
    s_octave_candidate_midi = -1;
    s_octave_consecutive = 0;
    reset_poly_history();
    s_template_spectrum_seen = false;
}

const char *low_frequency_reject_reason_name(
    low_frequency_reject_reason_t reason)
{
    switch (reason) {
        case LOW_FREQUENCY_REJECT_NONE: return "none";
        case LOW_FREQUENCY_REJECT_NOT_READY: return "low_not_ready";
        case LOW_FREQUENCY_REJECT_BELOW_BAND_GATE: return "below_band_gate";
        case LOW_FREQUENCY_REJECT_LOW_BAND_SNR: return "low_band_snr";
        case LOW_FREQUENCY_REJECT_YIN_LOW_CONFIDENCE: return "yin_low_confidence";
        case LOW_FREQUENCY_REJECT_HARMONIC_MISMATCH: return "harmonic_mismatch";
        case LOW_FREQUENCY_REJECT_OCTAVE_UNCONFIRMED: return "octave_unconfirmed";
        case LOW_FREQUENCY_REJECT_POLY_SINGLE_DOMINANT: return "poly_single_dominant";
        case LOW_FREQUENCY_REJECT_POLY_NO_INDEPENDENT_SUPPORT:
            return "poly_no_independent_support";
        case LOW_FREQUENCY_REJECT_STABILIZING: return "stabilizing";
        case LOW_FREQUENCY_REJECT_CLIPPING: return "clipping";
        case LOW_FREQUENCY_REJECT_SILENCE: return "silence";
        default: return "unknown";
    }
}

void low_frequency_analyzer_reset(void)
{
    memset(s_decimator_history, 0, sizeof(s_decimator_history));
    memset(s_low_ring, 0, sizeof(s_low_ring));
    memset(s_low_yin_window, 0, sizeof(s_low_yin_window));
    memset(s_low_magnitude, 0, sizeof(s_low_magnitude));
    s_decimator_position = 0;
    s_decimator_phase = 0;
    s_low_write_position = 0;
    s_low_filled = 0;
    reset_decision_history();
}

void low_frequency_analyzer_init(void)
{
    s_selected_mic = 0;
    low_frequency_analyzer_reset();
}

static float decimator_output(void)
{
    float output = 0.0f;
    size_t position = s_decimator_position;
    for (size_t tap = 0; tap < LOW_DECIMATOR_TAPS; ++tap) {
        position = position == 0 ? LOW_DECIMATOR_TAPS - 1 : position - 1;
        output += s_decimator_coefficients[tap] *
                  s_decimator_history[position];
    }
    return output;
}

void low_frequency_analyzer_push(const float *samples, size_t count,
                                 int selected_mic)
{
    if (samples == NULL || count == 0) return;
    if (selected_mic != s_selected_mic) {
        low_frequency_analyzer_reset();
        s_selected_mic = selected_mic;
    }
    for (size_t index = 0; index < count; ++index) {
        s_decimator_history[s_decimator_position] = samples[index];
        s_decimator_position =
            (s_decimator_position + 1) % LOW_DECIMATOR_TAPS;
        s_decimator_phase ^= 1U;
        if (s_decimator_phase != 0U) continue;
        s_low_ring[s_low_write_position] = decimator_output();
        s_low_write_position =
            (s_low_write_position + 1) % MUSIC_LOW_FFT_SIZE;
        if (s_low_filled < MUSIC_LOW_FFT_SIZE) ++s_low_filled;
    }
}

static void copy_low_yin_window(void)
{
    const size_t start =
        (s_low_write_position + MUSIC_LOW_FFT_SIZE -
         MUSIC_LOW_YIN_WINDOW_SIZE) % MUSIC_LOW_FFT_SIZE;
    for (size_t index = 0; index < MUSIC_LOW_YIN_WINDOW_SIZE; ++index) {
        s_low_yin_window[index] =
            s_low_ring[(start + index) % MUSIC_LOW_FFT_SIZE];
    }
}

static float goertzel_amplitude(float frequency_hz)
{
    if (frequency_hz <= 0.0f ||
        frequency_hz >= 0.5f * MUSIC_LOW_SAMPLE_RATE_HZ) {
        return 0.0f;
    }
    const float omega = 2.0f * (float)M_PI * frequency_hz /
                        MUSIC_LOW_SAMPLE_RATE_HZ;
    const float coefficient = 2.0f * cosf(omega);
    float previous = 0.0f;
    float previous2 = 0.0f;
    for (size_t index = 0; index < MUSIC_LOW_YIN_WINDOW_SIZE; ++index) {
        const float current = s_low_yin_window[index] +
                              coefficient * previous - previous2;
        previous2 = previous;
        previous = current;
    }
    const float power = previous2 * previous2 + previous * previous -
                        coefficient * previous * previous2;
    return 2.0f * sqrtf(fmaxf(0.0f, power)) /
           MUSIC_LOW_YIN_WINDOW_SIZE;
}

static float short_harmonic_score(float fundamental_hz,
                                  float *out_ratio,
                                  float *out_fundamental)
{
    double total_energy = 0.0;
    for (size_t index = 0; index < MUSIC_LOW_YIN_WINDOW_SIZE; ++index) {
        total_energy += (double)s_low_yin_window[index] *
                        s_low_yin_window[index];
    }
    const float rms_squared = (float)(total_energy /
        MUSIC_LOW_YIN_WINDOW_SIZE);
    float explained = 0.0f;
    float score = 0.0f;
    float fundamental = 0.0f;
    for (int harmonic = 1;
         harmonic <= MUSIC_LOW_TEMPLATE_HARMONICS; ++harmonic) {
        const float frequency_hz = fundamental_hz * harmonic;
        if (frequency_hz >= 0.5f * MUSIC_LOW_SAMPLE_RATE_HZ) break;
        const float amplitude = goertzel_amplitude(frequency_hz);
        if (harmonic == 1) fundamental = amplitude;
        const float weight = 1.0f / sqrtf((float)harmonic);
        score += weight * amplitude;
        explained += 0.5f * amplitude * amplitude;
    }
    *out_ratio = rms_squared > 1.0e-12f
        ? clamp01(explained / rms_squared) : 0.0f;
    *out_fundamental = fundamental;
    return score;
}

static float spectrum_noise_floor(int first_bin, int last_bin)
{
    uint16_t histogram[LOW_NOISE_HISTOGRAM_BINS] = {0};
    uint32_t total = 0;
    for (int bin = first_bin; bin <= last_bin; ++bin) {
        const float level = log10f(s_low_magnitude[bin] + 1.0e-12f);
        int slot = (int)((level + 8.0f) *
                         (LOW_NOISE_HISTOGRAM_BINS / 8.0f));
        if (slot < 0) slot = 0;
        if (slot >= LOW_NOISE_HISTOGRAM_BINS) {
            slot = LOW_NOISE_HISTOGRAM_BINS - 1;
        }
        ++histogram[slot];
        ++total;
    }
    const uint32_t target = total * 35U / 100U;
    uint32_t accumulated = 0;
    int slot = 0;
    for (; slot < LOW_NOISE_HISTOGRAM_BINS; ++slot) {
        accumulated += histogram[slot];
        if (accumulated >= target) break;
    }
    const float log_level =
        ((float)slot + 0.5f) * (8.0f / LOW_NOISE_HISTOGRAM_BINS) - 8.0f;
    return powf(10.0f, log_level);
}

static float local_peak(float frequency_hz, int *out_bin,
                        float *out_frequency, float noise_floor,
                        float *out_prominence)
{
    const float bin_hz = (float)MUSIC_LOW_SAMPLE_RATE_HZ /
                         MUSIC_LOW_FFT_SIZE;
    const int center = (int)lrintf(frequency_hz / bin_hz);
    int peak_bin = center;
    float peak = 0.0f;
    for (int offset = -1; offset <= 1; ++offset) {
        const int bin = center + offset;
        if (bin <= 1 || bin >= LOW_FFT_BIN_COUNT - 1) continue;
        if (s_low_magnitude[bin] > peak) {
            peak = s_low_magnitude[bin];
            peak_bin = bin;
        }
    }
    if (peak_bin <= 1 || peak_bin >= LOW_FFT_BIN_COUNT - 1) return 0.0f;
    const float left = s_low_magnitude[peak_bin - 1];
    const float right = s_low_magnitude[peak_bin + 1];
    const float denominator = left - 2.0f * peak + right;
    float offset = 0.0f;
    if (fabsf(denominator) > 1.0e-12f) {
        offset = 0.5f * (left - right) / denominator;
        offset = fmaxf(-0.5f, fminf(0.5f, offset));
    }
    if (out_bin != NULL) *out_bin = peak_bin;
    if (out_frequency != NULL) *out_frequency =
        ((float)peak_bin + offset) * bin_hz;
    if (out_prominence != NULL) *out_prominence = peak /
        fmaxf(noise_floor, fmaxf(left, right));
    return peak;
}

static int compare_template_candidates(const void *left, const void *right)
{
    const template_candidate_t *a = left;
    const template_candidate_t *b = right;
    return a->candidate.score < b->candidate.score ? 1 :
           a->candidate.score > b->candidate.score ? -1 : 0;
}

static float unique_support(const template_candidate_t *candidate,
                            const template_candidate_t *others,
                            int other_count)
{
    float total = 0.0f;
    float unique = 0.0f;
    const float base_frequency = candidate->candidate.frequency_hz;
    const float bin_hz = (float)MUSIC_LOW_SAMPLE_RATE_HZ /
                         MUSIC_LOW_FFT_SIZE;
    for (int harmonic = 1;
         harmonic <= MUSIC_LOW_TEMPLATE_HARMONICS; ++harmonic) {
        const float component = candidate->harmonic_peak[harmonic - 1] /
                                sqrtf((float)harmonic);
        total += component;
        bool shared = false;
        for (int other = 0; other < other_count && !shared; ++other) {
            for (int other_harmonic = 1;
                 other_harmonic <= MUSIC_LOW_TEMPLATE_HARMONICS;
                 ++other_harmonic) {
                const float distance = fabsf(
                    base_frequency * harmonic -
                    others[other].candidate.frequency_hz * other_harmonic);
                if (distance <= 1.25f * bin_hz) {
                    shared = true;
                    break;
                }
            }
        }
        if (!shared) unique += component;
    }
    if (total <= 1.0e-12f) return 0.0f;

    /* For octave-related notes every upper partial is shared. Estimate an
     * independently struck upper fundamental as excess above the lower note's
     * neighboring-harmonic envelope; a plain second harmonic gets no credit. */
    if (unique <= 1.0e-12f) {
        const float observed = candidate->harmonic_peak[0];
        for (int other = 0; other < other_count; ++other) {
            const float ratio = base_frequency /
                                others[other].candidate.frequency_hz;
            const int harmonic = (int)lrintf(ratio);
            if (harmonic < 2 || harmonic > MUSIC_LOW_TEMPLATE_HARMONICS ||
                fabsf(ratio - harmonic) > 0.04f) {
                continue;
            }
            const float before = harmonic > 1
                ? others[other].harmonic_peak[harmonic - 2] : 0.0f;
            const float after = harmonic < MUSIC_LOW_TEMPLATE_HARMONICS
                ? others[other].harmonic_peak[harmonic] : before;
            const float expected = sqrtf(fmaxf(0.0f, before * after));
            unique = fmaxf(unique,
                fmaxf(0.0f, observed -
                    MUSIC_LOW_HARMONIC_ENVELOPE_FACTOR * expected));
        }
    }
    return clamp01(unique / total);
}

static bool identify_triad(const template_candidate_t selected[3],
                           int *root, bool *minor)
{
    bool present[12] = {0};
    for (int index = 0; index < 3; ++index) {
        present[selected[index].candidate.midi % 12] = true;
    }
    for (int candidate_root = 0; candidate_root < 12; ++candidate_root) {
        if (present[candidate_root] && present[(candidate_root + 4) % 12] &&
            present[(candidate_root + 7) % 12]) {
            *root = candidate_root;
            *minor = false;
            return true;
        }
        if (present[candidate_root] && present[(candidate_root + 3) % 12] &&
            present[(candidate_root + 7) % 12]) {
            *root = candidate_root;
            *minor = true;
            return true;
        }
    }
    return false;
}

static int poly_identity(low_frequency_result_kind_t kind,
                         const template_candidate_t *selected,
                         int count, int chord_root, bool minor)
{
    if (kind == LOW_FREQUENCY_RESULT_CHORD) {
        return 100000 + chord_root * 2 + (minor ? 1 : 0);
    }
    int identity = 0;
    for (int index = 0; index < count; ++index) {
        identity = identity * 128 + selected[index].candidate.midi;
    }
    return identity;
}

static void analyze_template(bool onset, low_frequency_result_t *result)
{
    if (!chord_detector_fft_magnitude_window(
            s_low_ring, s_low_write_position, s_low_magnitude,
            LOW_FFT_BIN_COUNT)) {
        return;
    }
    result->spectrum_ready = true;
    s_template_spectrum_seen = true;
    const float bin_hz = (float)MUSIC_LOW_SAMPLE_RATE_HZ /
                         MUSIC_LOW_FFT_SIZE;
    const int first_bin = (int)ceilf(MUSIC_LOW_MIN_FREQUENCY_HZ / bin_hz);
    const int last_bin = (int)floorf(2000.0f / bin_hz);
    const float noise_floor = spectrum_noise_floor(first_bin, last_bin);
    memset(s_template_candidates, 0, sizeof(s_template_candidates));
    int count = 0;
    float best_score = 0.0f;
    for (int midi = MUSIC_LOW_MIDI_MIN; midi <= MUSIC_LOW_MIDI_MAX; ++midi) {
        template_candidate_t candidate = {0};
        candidate.candidate.midi = midi;
        const float nominal = note_midi_to_frequency(midi);
        int peak_bin = 0;
        float peak_frequency = 0.0f;
        float prominence = 0.0f;
        const float fundamental = local_peak(
            nominal, &peak_bin, &peak_frequency, noise_floor, &prominence);
        (void)peak_bin;
        candidate.candidate.frequency_hz = peak_frequency > 0.0f
            ? peak_frequency : nominal;
        candidate.candidate.prominence = prominence;
        candidate.independent_fundamental =
            fundamental >= noise_floor * MUSIC_LOW_TEMPLATE_NOISE_MULTIPLIER &&
            prominence >= MUSIC_LOW_TEMPLATE_MIN_PROMINENCE;
        float score = 0.0f;
        for (int harmonic = 1;
             harmonic <= MUSIC_LOW_TEMPLATE_HARMONICS; ++harmonic) {
            const float frequency_hz = nominal * harmonic;
            if (frequency_hz >= 0.5f * MUSIC_LOW_SAMPLE_RATE_HZ) break;
            const float peak = local_peak(frequency_hz, NULL, NULL,
                                          noise_floor, NULL);
            candidate.harmonic_peak[harmonic - 1] = peak;
            const float normalized = fmaxf(
                0.0f, peak / fmaxf(noise_floor, 1.0e-9f) - 1.0f);
            score += log1pf(normalized) / sqrtf((float)harmonic);
        }
        candidate.candidate.score = score;
        if (score <= 0.0f) continue;
        s_template_candidates[count++] = candidate;
        best_score = fmaxf(best_score, score);
    }
    if (count <= 0 || best_score <= 1.0e-12f) {
        reset_poly_history();
        return;
    }
    for (int index = 0; index < count; ++index) {
        s_template_candidates[index].candidate.relative_score =
            s_template_candidates[index].candidate.score / best_score;
    }
    qsort(s_template_candidates, (size_t)count,
          sizeof(s_template_candidates[0]), compare_template_candidates);
    result->debug_candidate_count =
        count < MUSIC_LOW_DEBUG_CANDIDATE_COUNT
            ? count : MUSIC_LOW_DEBUG_CANDIDATE_COUNT;
    for (int index = 0; index < result->debug_candidate_count; ++index) {
        result->debug_candidates[index] =
            s_template_candidates[index].candidate;
    }

    memset(s_active_candidates, 0, sizeof(s_active_candidates));
    int active_count = 0;
    for (int index = 0; index < count &&
         active_count < MUSIC_LOW_TEMPLATE_CANDIDATE_COUNT; ++index) {
        if (!s_template_candidates[index].independent_fundamental ||
            s_template_candidates[index].candidate.relative_score <
                MUSIC_LOW_TEMPLATE_MIN_RELATIVE) {
            continue;
        }
        s_active_candidates[active_count++] = s_template_candidates[index];
    }
    if (active_count < 2) {
        reset_poly_history();
        if (result->final_kind == LOW_FREQUENCY_RESULT_NONE) {
            result->reject_reason =
                LOW_FREQUENCY_REJECT_POLY_SINGLE_DOMINANT;
        }
        return;
    }

    float best_pair_score = 0.0f;
    template_candidate_t best_pair[2] = {0};
    float best_pair_unique[2] = {0};
    for (int first = 0; first < active_count; ++first) {
        for (int second = first + 1; second < active_count; ++second) {
            const template_candidate_t selected[2] = {
                s_active_candidates[first], s_active_candidates[second],
            };
            const float first_unique = unique_support(
                &selected[0], &selected[1], 1);
            const float second_unique = unique_support(
                &selected[1], &selected[0], 1);
            if (first_unique < MUSIC_LOW_POLY_MIN_UNIQUE_SUPPORT ||
                second_unique < MUSIC_LOW_POLY_MIN_UNIQUE_SUPPORT) {
                continue;
            }
            const float score = selected[0].candidate.score +
                                selected[1].candidate.score;
            if (score > best_pair_score) {
                best_pair_score = score;
                best_pair[0] = selected[0];
                best_pair[1] = selected[1];
                best_pair_unique[0] = first_unique;
                best_pair_unique[1] = second_unique;
            }
        }
    }
    const float pair_gain = best_pair_score > 1.0e-12f
        ? (best_pair_score - best_score) / best_pair_score : 0.0f;
    if (best_pair_score <= 0.0f || pair_gain < MUSIC_LOW_POLY_MIN_GAIN) {
        reset_poly_history();
        if (result->final_kind == LOW_FREQUENCY_RESULT_NONE) {
            result->reject_reason = active_count >= 2
                ? LOW_FREQUENCY_REJECT_POLY_NO_INDEPENDENT_SUPPORT
                : LOW_FREQUENCY_REJECT_POLY_SINGLE_DOMINANT;
        }
        return;
    }

    template_candidate_t best_triad[3] = {0};
    float best_triad_score = 0.0f;
    int best_root = -1;
    bool best_minor = false;
    for (int first = 0; first < active_count; ++first) {
        for (int second = first + 1; second < active_count; ++second) {
            for (int third = second + 1; third < active_count; ++third) {
                template_candidate_t selected[3] = {
                    s_active_candidates[first], s_active_candidates[second],
                    s_active_candidates[third],
                };
                int root = -1;
                bool minor = false;
                if (!identify_triad(selected, &root, &minor)) continue;
                bool independent = true;
                for (int index = 0; index < 3; ++index) {
                    template_candidate_t others[2] = {
                        selected[(index + 1) % 3],
                        selected[(index + 2) % 3],
                    };
                    const float unique = unique_support(
                        &selected[index], others, 2);
                    if (unique < MUSIC_LOW_POLY_MIN_UNIQUE_SUPPORT) {
                        independent = false;
                        break;
                    }
                }
                if (!independent) continue;
                const float score = selected[0].candidate.score +
                                    selected[1].candidate.score +
                                    selected[2].candidate.score;
                if (score > best_triad_score) {
                    best_triad_score = score;
                    memcpy(best_triad, selected, sizeof(best_triad));
                    best_root = root;
                    best_minor = minor;
                }
            }
        }
    }

    low_frequency_result_kind_t kind = LOW_FREQUENCY_RESULT_INTERVAL;
    template_candidate_t selected[3] = {best_pair[0], best_pair[1]};
    int selected_count = 2;
    if (best_triad_score > best_pair_score * 1.12f && best_root >= 0) {
        kind = LOW_FREQUENCY_RESULT_CHORD;
        memcpy(selected, best_triad, sizeof(selected));
        selected_count = 3;
    }
    for (int left = 0; left < selected_count; ++left) {
        for (int right = left + 1; right < selected_count; ++right) {
            if (selected[right].candidate.midi <
                selected[left].candidate.midi) {
                const template_candidate_t swap = selected[left];
                selected[left] = selected[right];
                selected[right] = swap;
            }
        }
    }
    const int identity = poly_identity(
        kind, selected, selected_count, best_root, best_minor);
    if (identity == s_previous_poly_identity) {
        ++s_poly_consecutive;
    } else {
        s_previous_poly_identity = identity;
        s_poly_consecutive = 1;
    }
    result->candidate_kind = kind;
    result->note_count = selected_count;
    for (int index = 0; index < selected_count; ++index) {
        result->midi_notes[index] = selected[index].candidate.midi;
    }
    const float weakest_relative = selected_count == 3
        ? fminf(selected[0].candidate.relative_score,
                fminf(selected[1].candidate.relative_score,
                      selected[2].candidate.relative_score))
        : fminf(selected[0].candidate.relative_score,
                selected[1].candidate.relative_score);
    result->confidence = clamp01(0.45f + 0.35f * pair_gain +
                                 0.20f * weakest_relative);
    if (kind == LOW_FREQUENCY_RESULT_CHORD) {
        result->chord_root = best_root;
        result->chord_is_minor = best_minor;
        snprintf(result->chord_name, sizeof(result->chord_name), "%s:%s",
                 note_pitch_class_name(best_root),
                 best_minor ? "min" : "maj");
    }
    if (s_poly_consecutive >= MUSIC_LOW_POLY_CONFIRM_FRAMES ||
        (onset && s_poly_consecutive >=
            MUSIC_LOW_POLY_CONFIRM_FRAMES - 1)) {
        result->final_kind = kind;
        result->reject_reason = LOW_FREQUENCY_REJECT_NONE;
    } else {
        result->reject_reason = LOW_FREQUENCY_REJECT_STABILIZING;
    }
    (void)best_pair_unique;
}

void low_frequency_analyzer_analyze(bool triggered, bool run_spectrum,
                                    bool low_band_above_gate,
                                    bool clipped, bool onset,
                                    float low_band_noise_floor,
                                    const float band_snr_db[
                                        AUDIO_PREPROCESS_BAND_COUNT],
                                    low_frequency_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->yin.midi = -1;
    result->chord_root = -1;
    for (int index = 0; index < 3; ++index) result->midi_notes[index] = -1;
    result->triggered = triggered;
    result->reject_reason = LOW_FREQUENCY_REJECT_NOT_READY;
    if (band_snr_db != NULL) {
        memcpy(result->band_snr_db, band_snr_db,
               sizeof(result->band_snr_db));
    }
    if (!triggered) {
        reset_decision_history();
        return;
    }
    if (clipped) {
        reset_decision_history();
        result->reject_reason = LOW_FREQUENCY_REJECT_CLIPPING;
        return;
    }
    if (!low_band_above_gate) {
        reset_decision_history();
        result->reject_reason = LOW_FREQUENCY_REJECT_BELOW_BAND_GATE;
        return;
    }
    if (result->band_snr_db[AUDIO_PREPROCESS_BAND_LOW] <
        MUSIC_LOW_BAND_MIN_SNR_DB) {
        reset_decision_history();
        result->reject_reason = LOW_FREQUENCY_REJECT_LOW_BAND_SNR;
        return;
    }
    if (s_low_filled < MUSIC_LOW_YIN_WINDOW_SIZE) return;
    result->ready = true;
    copy_low_yin_window();
    int64_t started = esp_timer_get_time();
    yin_detector_analyze_range(
        s_low_yin_window, MUSIC_LOW_YIN_WINDOW_SIZE,
        MUSIC_LOW_SAMPLE_RATE_HZ, MUSIC_LOW_MIN_FREQUENCY_HZ,
        MUSIC_LOW_MAX_FREQUENCY_HZ, MUSIC_YIN_THRESHOLD, &result->yin);
    result->yin_time_us = (uint32_t)(esp_timer_get_time() - started);
    if (!result->yin.valid) {
        s_octave_candidate_midi = -1;
        s_octave_consecutive = 0;
        result->reject_reason = LOW_FREQUENCY_REJECT_YIN_LOW_CONFIDENCE;
    } else {
        float fundamental = 0.0f;
        const float direct_score = short_harmonic_score(
            result->yin.frequency_hz, &result->harmonic_ratio,
            &fundamental);
        bool octave_pending = false;
        if (result->yin.midi - 12 >= MUSIC_LOW_MIDI_MIN) {
            float half_ratio = 0.0f;
            float half_fundamental = 0.0f;
            const float half_score = short_harmonic_score(
                result->yin.frequency_hz * 0.5f, &half_ratio,
                &half_fundamental);
            const bool continuity_support =
                s_previous_single_midi == result->yin.midi - 12;
            const bool octave_evidence = result->yin.confidence >=
                    MUSIC_LOW_OCTAVE_MIN_CONFIDENCE &&
                half_fundamental >= low_band_noise_floor *
                    MUSIC_LOW_TEMPLATE_NOISE_MULTIPLIER &&
                (half_score >= direct_score * MUSIC_LOW_OCTAVE_SCORE_MARGIN ||
                  (continuity_support &&
                   half_score >= direct_score * 0.65f));
            if (octave_evidence) {
                const int lower_midi = result->yin.midi - 12;
                if (s_octave_candidate_midi == lower_midi) {
                    ++s_octave_consecutive;
                } else {
                    s_octave_candidate_midi = lower_midi;
                    s_octave_consecutive = 1;
                }
                const bool octave_confirmed =
                    s_octave_consecutive >= MUSIC_LOW_OCTAVE_CONFIRM_FRAMES ||
                    (onset && s_octave_consecutive >=
                        MUSIC_LOW_OCTAVE_CONFIRM_FRAMES - 1);
                octave_pending = !octave_confirmed;
                if (octave_confirmed) {
                result->yin.frequency_hz *= 0.5f;
                result->yin.midi -= 12;
                result->yin.cents = note_cents_error(
                    result->yin.frequency_hz, result->yin.midi);
                note_midi_to_name(result->yin.midi,
                                  result->yin.note_name,
                                  sizeof(result->yin.note_name));
                result->harmonic_ratio = half_ratio;
                result->octave_corrected = true;
                result->octave_shift = -12;
                }
            } else {
                s_octave_candidate_midi = -1;
                s_octave_consecutive = 0;
            }
        } else {
            s_octave_candidate_midi = -1;
            s_octave_consecutive = 0;
        }
        const float continuity_cents = s_previous_single_frequency > 0.0f
            ? fabsf(1200.0f * log2f(result->yin.frequency_hz /
                                     s_previous_single_frequency))
            : 0.0f;
        bool unconfirmed_octave = octave_pending;
        if (result->yin.midi == s_previous_single_midi &&
            continuity_cents <= MUSIC_LOW_CONTINUITY_CENTS) {
            ++s_single_consecutive;
        } else {
            unconfirmed_octave = unconfirmed_octave ||
                (s_previous_single_midi >= MUSIC_LOW_MIDI_MIN &&
                 abs(result->yin.midi - s_previous_single_midi) == 12 &&
                 !onset);
            s_previous_single_midi = result->yin.midi;
            s_single_consecutive = unconfirmed_octave ? 0 : 1;
        }
        s_previous_single_frequency = result->yin.frequency_hz;
        const bool high_confidence =
            result->yin.confidence >= MUSIC_LOW_YIN_CONFIDENCE_THRESHOLD &&
            result->harmonic_ratio >=
                MUSIC_LOW_HARMONIC_RATIO_THRESHOLD &&
            (!unconfirmed_octave || result->octave_corrected);
        const bool continuous =
            result->yin.confidence >=
                MUSIC_LOW_YIN_CONTINUITY_CONFIDENCE &&
            result->harmonic_ratio >=
                MUSIC_LOW_CONTINUITY_HARMONIC_RATIO &&
            s_single_consecutive >= MUSIC_LOW_SINGLE_CONFIRM_FRAMES &&
            (!unconfirmed_octave || result->octave_corrected);
        result->candidate_kind = LOW_FREQUENCY_RESULT_SINGLE;
        result->midi_notes[0] = result->yin.midi;
        result->note_count = 1;
        result->confidence = fminf(result->yin.confidence,
                                   result->harmonic_ratio);
        if (high_confidence || continuous) {
            result->final_kind = LOW_FREQUENCY_RESULT_SINGLE;
            result->reject_reason = LOW_FREQUENCY_REJECT_NONE;
        } else if (unconfirmed_octave) {
            result->reject_reason =
                LOW_FREQUENCY_REJECT_OCTAVE_UNCONFIRMED;
        } else if (result->yin.confidence <
                   MUSIC_LOW_YIN_CONTINUITY_CONFIDENCE) {
            result->reject_reason =
                LOW_FREQUENCY_REJECT_YIN_LOW_CONFIDENCE;
        } else if (result->harmonic_ratio <
                   MUSIC_LOW_CONTINUITY_HARMONIC_RATIO) {
            result->reject_reason =
                LOW_FREQUENCY_REJECT_HARMONIC_MISMATCH;
        } else {
            result->reject_reason = LOW_FREQUENCY_REJECT_STABILIZING;
        }
        (void)fundamental;
    }

    const bool clear_single =
        result->final_kind == LOW_FREQUENCY_RESULT_SINGLE &&
        result->yin.confidence >= MUSIC_LOW_SINGLE_FFT_SKIP_CONFIDENCE &&
        result->harmonic_ratio >=
            MUSIC_LOW_SINGLE_FFT_SKIP_HARMONIC_RATIO &&
        !result->octave_corrected;
    if (clear_single) reset_poly_history();
    if (run_spectrum && s_low_filled >= MUSIC_LOW_FFT_SIZE &&
        (onset || !clear_single || !s_template_spectrum_seen)) {
        started = esp_timer_get_time();
        analyze_template(onset, result);
        result->spectrum_time_us =
            (uint32_t)(esp_timer_get_time() - started);
    }
}
