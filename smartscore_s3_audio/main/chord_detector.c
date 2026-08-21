#include "chord_detector.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsps_fft2r.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "diagnostics.h"
#include "music_detector_config.h"
#include "note_utils.h"

#define FFT_BIN_COUNT (MUSIC_FFT_SIZE / 2 + 1)
#define CHORD_NOTE_COUNT (MUSIC_CHORD_MIDI_MAX - MUSIC_CHORD_MIDI_MIN + 1)
#define NOISE_HISTOGRAM_BINS 48
#define SPECTRUM_NOISE_BAND_COUNT 3
#define EXACT_HARMONIC_RESIDUAL_MIN_SALIENCE 0.65f

static float s_fft_work[MUSIC_FFT_SIZE * 2];
#if !MUSIC_USE_SINGLE_MIC_CH1
static float s_mic1_magnitude[FFT_BIN_COUNT];
static float s_mic2_magnitude[FFT_BIN_COUNT];
#endif
static float s_fused_magnitude[FFT_BIN_COUNT];
static float s_hann[MUSIC_FFT_SIZE];
static float s_fft_magnitude_scale = 1.0f;
static bool s_harmonic_bin_selected[FFT_BIN_COUNT];
static bool s_initialized;

/* Exact 61-key evidence scratch. Kept separate from the legacy 48..84
 * classification arrays so the 613089a chord/mid-high path stays untouched. */
static float s_ev_fundamental_score[CHORD_PIANO_KEY_COUNT];
static float s_ev_fundamental_noise_floor[CHORD_PIANO_KEY_COUNT];
static int s_ev_fundamental_peak_bin[CHORD_PIANO_KEY_COUNT];
static float s_ev_fundamental_peak_frequency[CHORD_PIANO_KEY_COUNT];
static float s_ev_fundamental_prominence[CHORD_PIANO_KEY_COUNT];
static float s_ev_raw_score[CHORD_PIANO_KEY_COUNT];
static float s_ev_piano_score[CHORD_PIANO_KEY_COUNT];
static bool s_ev_harmonic_owned[CHORD_PIANO_KEY_COUNT];

static float clamp01(float value)
{
    return fmaxf(0.0f, fminf(1.0f, value));
}

int chord_detector_init(void)
{
    if (s_initialized) return ESP_OK;
    float window_sum = 0.0f;
    for (int i = 0; i < MUSIC_FFT_SIZE; ++i) {
        s_hann[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (MUSIC_FFT_SIZE - 1));
        window_sum += s_hann[i];
    }
    /* Normalize the one-sided FFT back to signal amplitude. Without this,
     * the 4096-point transform saturates the logarithmic noise histogram and
     * reports the fixed 0.825405 floor seen in the failing hardware log. */
    if (window_sum > 1.0e-9f) {
        s_fft_magnitude_scale = 2.0f / window_sum;
    }
    const esp_err_t error = dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE);
    if (error != ESP_OK) return error;
    s_initialized = true;
    return ESP_OK;
}

static void fft_magnitude(const float *ring, size_t write_position, float *magnitude)
{
    for (size_t i = 0; i < MUSIC_FFT_SIZE; ++i) {
        const size_t ring_index = (write_position + i) % MUSIC_FFT_SIZE;
        s_fft_work[2 * i] = ring[ring_index] * s_hann[i];
        s_fft_work[2 * i + 1] = 0.0f;
    }
    dsps_fft2r_fc32(s_fft_work, MUSIC_FFT_SIZE);
    dsps_bit_rev_fc32(s_fft_work, MUSIC_FFT_SIZE);
    for (int bin = 0; bin < FFT_BIN_COUNT; ++bin) {
        const float real = s_fft_work[2 * bin];
        const float imaginary = s_fft_work[2 * bin + 1];
        magnitude[bin] = sqrtf(real * real + imaginary * imaginary) *
                         s_fft_magnitude_scale;
    }
}

static float estimate_noise_floor(const float *magnitude, int first_bin, int last_bin)
{
    uint16_t histogram[NOISE_HISTOGRAM_BINS] = {0};
    uint32_t total = 0;
    for (int bin = first_bin; bin <= last_bin; ++bin) {
        const float level = log10f(magnitude[bin] + 1.0e-12f);
        int index = (int)((level + 8.0f) * (NOISE_HISTOGRAM_BINS / 8.0f));
        if (index < 0) index = 0;
        if (index >= NOISE_HISTOGRAM_BINS) index = NOISE_HISTOGRAM_BINS - 1;
        ++histogram[index];
        ++total;
    }
    const uint32_t target = total * 35 / 100;
    uint32_t accumulated = 0;
    int index = 0;
    for (; index < NOISE_HISTOGRAM_BINS; ++index) {
        accumulated += histogram[index];
        if (accumulated >= target) break;
    }
    const float log_level = ((float)index + 0.5f) * (8.0f / NOISE_HISTOGRAM_BINS) - 8.0f;
    return powf(10.0f, log_level);
}

static int noise_band_for_frequency(float frequency_hz)
{
    if (frequency_hz < MUSIC_SPECTRUM_LOW_BAND_MAX_HZ) return 0;
    if (frequency_hz < MUSIC_SPECTRUM_MID_BAND_MAX_HZ) return 1;
    return 2;
}

static float band_noise_for_frequency(const float band_noise[3],
                                      float frequency_hz)
{
    return band_noise[noise_band_for_frequency(frequency_hz)];
}

static float local_peak(float frequency_hz)
{
    const float bin_frequency = (float)MUSIC_SAMPLE_RATE_HZ / MUSIC_FFT_SIZE;
    const int center = (int)lrintf(frequency_hz / bin_frequency);
    float maximum = 0.0f;
    for (int offset = -1; offset <= 1; ++offset) {
        const int bin = center + offset;
        if (bin > 0 && bin < FFT_BIN_COUNT && s_fused_magnitude[bin] > maximum) {
            maximum = s_fused_magnitude[bin];
        }
    }
    return maximum;
}

static float local_peak_snr(float frequency_hz, const float band_noise[3])
{
    if (frequency_hz <= 0.0f ||
        frequency_hz > MUSIC_SPECTRUM_MAX_FREQUENCY_HZ) {
        return 0.0f;
    }
    const float noise = band_noise_for_frequency(band_noise, frequency_hz);
    return local_peak(frequency_hz) / fmaxf(noise, 1.0e-9f);
}

static float interpolated_peak_frequency(int peak_bin)
{
    const float bin_frequency = (float)MUSIC_SAMPLE_RATE_HZ / MUSIC_FFT_SIZE;
    const float left = s_fused_magnitude[peak_bin - 1];
    const float center = s_fused_magnitude[peak_bin];
    const float right = s_fused_magnitude[peak_bin + 1];
    const float denominator = left - 2.0f * center + right;
    float offset = 0.0f;
    if (fabsf(denominator) > 1.0e-12f) {
        offset = 0.5f * (left - right) / denominator;
        offset = fmaxf(-0.5f, fminf(0.5f, offset));
    }
    return ((float)peak_bin + offset) * bin_frequency;
}

void chord_detector_analyze(const float *mic1_ring, const float *mic2_ring,
                            size_t write_position, float mic1_weight,
                            float mic2_weight, bool demo_profile,
                            float signal_snr_db, int low_anchor_midi,
                            float low_anchor_confidence,
                            const low_note_result_t *low_notes,
                            chord_result_t *result)
{
    float fundamental_relative_threshold = MUSIC_FUNDAMENTAL_RELATIVE_THRESHOLD;
    float active_pitch_class_threshold = MUSIC_ACTIVE_PITCH_CLASS_THRESHOLD;
    float chord_confidence_threshold = MUSIC_CHORD_CONFIDENCE_THRESHOLD;
    float interval_confidence_threshold = MUSIC_INTERVAL_CONFIDENCE_THRESHOLD;
    int interval_max_active_classes = MUSIC_INTERVAL_MAX_ACTIVE_CLASSES;
    if (demo_profile && signal_snr_db >= MUSIC_DEMO_SNR_HIGH_DB) {
        fundamental_relative_threshold = MUSIC_DEMO_HIGH_SNR_RELATIVE_THRESHOLD;
        active_pitch_class_threshold = MUSIC_DEMO_HIGH_SNR_ACTIVE_THRESHOLD;
        chord_confidence_threshold = MUSIC_DEMO_HIGH_SNR_CHORD_THRESHOLD;
        interval_confidence_threshold = MUSIC_DEMO_HIGH_SNR_INTERVAL_THRESHOLD;
        interval_max_active_classes = MUSIC_DEMO_INTERVAL_MAX_ACTIVE_CLASSES;
    } else if (demo_profile && signal_snr_db >= MUSIC_DEMO_SNR_MEDIUM_DB) {
        fundamental_relative_threshold = MUSIC_DEMO_MEDIUM_SNR_RELATIVE_THRESHOLD;
        active_pitch_class_threshold = MUSIC_DEMO_MEDIUM_SNR_ACTIVE_THRESHOLD;
        chord_confidence_threshold = MUSIC_DEMO_MEDIUM_SNR_CHORD_THRESHOLD;
        interval_confidence_threshold = MUSIC_DEMO_MEDIUM_SNR_INTERVAL_THRESHOLD;
        interval_max_active_classes = MUSIC_DEMO_INTERVAL_MAX_ACTIVE_CLASSES;
    } else if (demo_profile && signal_snr_db < MUSIC_DEMO_SNR_MIN_DB) {
        /* Preserve spectrum diagnostics but never promote an unreliable frame
         * to a polyphonic result below the minimum usable SNR. */
        chord_confidence_threshold = 2.0f;
        interval_confidence_threshold = 2.0f;
    }
    memset(result, 0, sizeof(*result));
    for (int i = 0; i < 4; ++i) result->midi_notes[i] = -1;
    for (int i = 0; i < CHORD_EXACT_NOTE_COUNT; ++i) {
        result->exact_midi_notes[i] = -1;
    }
    for (int i = 0; i < CHORD_DEBUG_CANDIDATE_COUNT; ++i) {
        result->debug_candidates[i].midi = -1;
    }
    if (!s_initialized || mic1_ring == NULL) return;
#if !MUSIC_USE_SINGLE_MIC_CH1
    if (mic2_ring == NULL) return;
#endif

    const int first_bin = (int)ceilf(MUSIC_MIN_FREQUENCY_HZ * MUSIC_FFT_SIZE / MUSIC_SAMPLE_RATE_HZ);
    const int last_bin = (int)floorf(MUSIC_MAX_FREQUENCY_HZ * MUSIC_FFT_SIZE / MUSIC_SAMPLE_RATE_HZ);
    int64_t fft_started = esp_timer_get_time();
#if MUSIC_USE_SINGLE_MIC_CH1
    (void)mic2_ring;
    (void)mic1_weight;
    (void)mic2_weight;
    fft_magnitude(mic1_ring, write_position, s_fused_magnitude);
    diagnostics_counters()->mic1_fft_time_us = (uint32_t)(esp_timer_get_time() - fft_started);
    diagnostics_counters()->mic2_fft_time_us = 0;
#else
    fft_magnitude(mic1_ring, write_position, s_mic1_magnitude);
    diagnostics_counters()->mic1_fft_time_us = (uint32_t)(esp_timer_get_time() - fft_started);
    fft_started = esp_timer_get_time();
    fft_magnitude(mic2_ring, write_position, s_mic2_magnitude);
    diagnostics_counters()->mic2_fft_time_us = (uint32_t)(esp_timer_get_time() - fft_started);
    const float weight_sum = mic1_weight + mic2_weight;
    const float fallback_weight = demo_profile ? 0.0f : 0.5f;
    const float weight1 = weight_sum > 1.0e-9f
                              ? mic1_weight / weight_sum : fallback_weight;
    const float weight2 = weight_sum > 1.0e-9f
                              ? mic2_weight / weight_sum : fallback_weight;
    for (int bin = 0; bin < FFT_BIN_COUNT; ++bin) {
        s_fused_magnitude[bin] = weight1 * s_mic1_magnitude[bin] + weight2 * s_mic2_magnitude[bin];
    }
#endif
    const float bin_frequency = (float)MUSIC_SAMPLE_RATE_HZ / MUSIC_FFT_SIZE;
    const int low_last_bin = (int)floorf(MUSIC_SPECTRUM_LOW_BAND_MAX_HZ /
                                         bin_frequency);
    const int mid_last_bin = (int)floorf(MUSIC_SPECTRUM_MID_BAND_MAX_HZ /
                                         bin_frequency);
    const int band_first[SPECTRUM_NOISE_BAND_COUNT] = {
        first_bin, low_last_bin + 1, mid_last_bin + 1,
    };
    const int band_last[SPECTRUM_NOISE_BAND_COUNT] = {
        low_last_bin, mid_last_bin, last_bin,
    };
    float band_noise[SPECTRUM_NOISE_BAND_COUNT];
    for (int band = 0; band < SPECTRUM_NOISE_BAND_COUNT; ++band) {
        band_noise[band] = estimate_noise_floor(
            s_fused_magnitude, band_first[band], band_last[band]);
        result->band_noise_floor[band] = band_noise[band];
    }
    const float noise_floor = estimate_noise_floor(s_fused_magnitude,
                                                   first_bin, last_bin);
    if (!demo_profile) {
        for (int band = 0; band < SPECTRUM_NOISE_BAND_COUNT; ++band) {
            band_noise[band] = noise_floor;
            result->band_noise_floor[band] = noise_floor;
        }
    }
    for (int bin = 0; bin < FFT_BIN_COUNT; ++bin) {
        const float frequency_hz = bin * bin_frequency;
        if (s_fused_magnitude[bin] <
            band_noise_for_frequency(band_noise, frequency_hz) * 1.8f) {
            s_fused_magnitude[bin] = 0.0f;
        }
    }

    float raw_score[CHORD_NOTE_COUNT] = {0};
    float independent_score[CHORD_NOTE_COUNT] = {0};
    float fundamental_score[CHORD_NOTE_COUNT] = {0};
    float fundamental_noise_floor[CHORD_NOTE_COUNT] = {0};
    int fundamental_peak_bin[CHORD_NOTE_COUNT] = {0};
    float fundamental_peak_frequency[CHORD_NOTE_COUNT] = {0};
    float fundamental_prominence[CHORD_NOTE_COUNT] = {0};
    result->spectrum_noise_floor = noise_floor;

    /* Extract physical local maxima first, then map each peak to exactly one
     * MIDI note. The previous note-centric search could assign the two sides
     * of one Hann main lobe to adjacent semitones (for example C4 and B3). */
    const int first_note_bin = (int)floorf(note_midi_to_frequency(MUSIC_CHORD_MIDI_MIN) /
                                           bin_frequency) - 1;
    const int last_note_bin = (int)ceilf(note_midi_to_frequency(MUSIC_CHORD_MIDI_MAX) /
                                         bin_frequency) + 1;
    for (int bin = first_note_bin; bin <= last_note_bin; ++bin) {
        if (bin <= 1 || bin >= FFT_BIN_COUNT - 1) continue;
        const float left = s_fused_magnitude[bin - 1];
        const float center = s_fused_magnitude[bin];
        const float right = s_fused_magnitude[bin + 1];
        const float bin_noise = band_noise_for_frequency(
            band_noise, bin * bin_frequency);
        if (center < bin_noise * MUSIC_FUNDAMENTAL_NOISE_MULTIPLIER ||
            !(center > left && center >= right)) {
            continue;
        }
        const float peak_frequency = interpolated_peak_frequency(bin);
        const int midi = note_frequency_to_midi(peak_frequency);
        if (midi < MUSIC_CHORD_MIDI_MIN || midi > MUSIC_CHORD_MIDI_MAX) continue;
        const int note = midi - MUSIC_CHORD_MIDI_MIN;
        if (center <= fundamental_score[note]) continue;
        fundamental_score[note] = center;
        fundamental_noise_floor[note] = bin_noise;
        fundamental_peak_bin[note] = bin;
        fundamental_peak_frequency[note] = peak_frequency;
        fundamental_prominence[note] = fminf(99.0f, center /
            fmaxf(bin_noise, fmaxf(left, right)));
    }

    for (int note = 0; note < CHORD_NOTE_COUNT; ++note) {
        const float f0 = note_midi_to_frequency(MUSIC_CHORD_MIDI_MIN + note);
        raw_score[note] = fundamental_score[note];
        for (int harmonic = 2; harmonic <= 5; ++harmonic) {
            const float frequency = f0 * harmonic;
            if (frequency > MUSIC_MAX_FREQUENCY_HZ) break;
            raw_score[note] += MUSIC_HARMONIC_SUPPORT_WEIGHT * local_peak(frequency) / harmonic;
        }
    }

    float maximum_fundamental = 0.0f;
    for (int note = 0; note < CHORD_NOTE_COUNT; ++note) {
        maximum_fundamental = fmaxf(maximum_fundamental, fundamental_score[note]);
    }

    bool harmonic_owned[CHORD_NOTE_COUNT] = {0};
    for (int note = 0; note < CHORD_NOTE_COUNT; ++note) {
        const float note_frequency = note_midi_to_frequency(
            MUSIC_CHORD_MIDI_MIN + note);
        if (fundamental_noise_floor[note] <= 0.0f) {
            fundamental_noise_floor[note] =
                band_noise_for_frequency(band_noise, note_frequency);
        }
        if (fundamental_score[note] < fundamental_noise_floor[note] *
                                          MUSIC_FUNDAMENTAL_NOISE_MULTIPLIER ||
            fundamental_score[note] < maximum_fundamental * fundamental_relative_threshold) {
            continue;
        }
        for (int lower = 0; lower < note; ++lower) {
            if (harmonic_owned[lower] || fundamental_peak_frequency[lower] <= 0.0f ||
                fundamental_score[lower] < fundamental_noise_floor[lower] *
                                               MUSIC_FUNDAMENTAL_NOISE_MULTIPLIER ||
                fundamental_score[lower] < maximum_fundamental * fundamental_relative_threshold ||
                fundamental_score[lower] <
                    fundamental_score[note] * MUSIC_HARMONIC_OWNER_MIN_RELATIVE) {
                continue;
            }
            const float ratio = fundamental_peak_frequency[note] /
                                fundamental_peak_frequency[lower];
            const int harmonic = (int)lrintf(ratio);
            if (harmonic >= 2 && harmonic <= 5 &&
                fabsf(ratio - harmonic) <= MUSIC_HARMONIC_RATIO_TOLERANCE) {
                harmonic_owned[note] = true;
                ++result->harmonic_rejected_count;
                break;
            }
        }
    }

    for (int note = 0; note < CHORD_NOTE_COUNT; ++note) {
        const float f0 = note_midi_to_frequency(MUSIC_CHORD_MIDI_MIN + note);
        /* A candidate must have energy at its own fundamental. Without this
         * condition, a real C4 peak also scores C3/C2 as if it were their
         * second/fourth harmonic and creates many false pitch classes. */
        if (harmonic_owned[note] ||
            fundamental_score[note] < fundamental_noise_floor[note] *
                                          MUSIC_FUNDAMENTAL_NOISE_MULTIPLIER ||
            fundamental_score[note] < maximum_fundamental * fundamental_relative_threshold) {
            independent_score[note] = 0.0f;
            continue;
        }
        float explained = 0.0f;
        for (int lower = 0; lower < note; ++lower) {
            const float lower_f0 = note_midi_to_frequency(MUSIC_CHORD_MIDI_MIN + lower);
            const float ratio = f0 / lower_f0;
            const int harmonic = (int)lrintf(ratio);
            const bool lower_has_fundamental = !harmonic_owned[lower] &&
                fundamental_score[lower] >= fundamental_noise_floor[lower] *
                                               MUSIC_FUNDAMENTAL_NOISE_MULTIPLIER &&
                fundamental_score[lower] >= maximum_fundamental * fundamental_relative_threshold;
            if (lower_has_fundamental && harmonic >= 2 && harmonic <= 5 &&
                fabsf(ratio - harmonic) < 0.025f) {
                explained = fmaxf(explained, raw_score[lower] / sqrtf((float)harmonic));
            }
        }
        independent_score[note] = fmaxf(0.0f, raw_score[note] -
                                               MUSIC_HARMONIC_SUPPRESSION_WEIGHT * explained);
    }

    float maximum_independent_score = 0.0f;
    for (int note = 0; note < CHORD_NOTE_COUNT; ++note) {
        maximum_independent_score = fmaxf(maximum_independent_score, independent_score[note]);
    }

    /* Exact 61-key evidence layer (additive). A 256 ms piano-tuned low-note
     * matcher resolves C2..B3 using several harmonics where the 4096-point
     * 24 kHz FFT cannot separate adjacent fundamentals. This layer feeds the
     * piano note tracker only; the legacy classifier above is untouched. */
    memset(s_ev_fundamental_score, 0, sizeof(s_ev_fundamental_score));
    memset(s_ev_fundamental_noise_floor, 0,
           sizeof(s_ev_fundamental_noise_floor));
    memset(s_ev_fundamental_peak_bin, 0, sizeof(s_ev_fundamental_peak_bin));
    memset(s_ev_fundamental_peak_frequency, 0,
           sizeof(s_ev_fundamental_peak_frequency));
    memset(s_ev_fundamental_prominence, 0,
           sizeof(s_ev_fundamental_prominence));
    memset(s_ev_raw_score, 0, sizeof(s_ev_raw_score));
    memset(s_ev_piano_score, 0, sizeof(s_ev_piano_score));
    memset(s_ev_harmonic_owned, 0, sizeof(s_ev_harmonic_owned));
    const int evidence_first_note_bin =
        (int)floorf(note_midi_to_frequency(MUSIC_PIANO_MIDI_MIN) /
                    bin_frequency) - 1;
    const int evidence_last_note_bin =
        (int)ceilf(note_midi_to_frequency(MUSIC_PIANO_MIDI_MAX) /
                   bin_frequency) + 1;
    for (int bin = evidence_first_note_bin; bin <= evidence_last_note_bin;
         ++bin) {
        if (bin <= 1 || bin >= FFT_BIN_COUNT - 1) continue;
        const float left = s_fused_magnitude[bin - 1];
        const float center = s_fused_magnitude[bin];
        const float right = s_fused_magnitude[bin + 1];
        const float bin_noise = band_noise_for_frequency(
            band_noise, bin * bin_frequency);
        if (center < bin_noise * MUSIC_FUNDAMENTAL_NOISE_MULTIPLIER ||
            !(center > left && center >= right)) {
            continue;
        }
        const float peak_frequency = interpolated_peak_frequency(bin);
        const int midi = note_frequency_to_midi(peak_frequency);
        if (midi < MUSIC_PIANO_MIDI_MIN || midi > MUSIC_PIANO_MIDI_MAX) {
            continue;
        }
        const int note = midi - MUSIC_PIANO_MIDI_MIN;
        if (center <= s_ev_fundamental_score[note]) continue;
        s_ev_fundamental_score[note] = center;
        s_ev_fundamental_noise_floor[note] = bin_noise;
        s_ev_fundamental_peak_bin[note] = bin;
        s_ev_fundamental_peak_frequency[note] = peak_frequency;
        s_ev_fundamental_prominence[note] = fminf(99.0f, center /
            fmaxf(bin_noise, fmaxf(left, right)));
    }
    for (int note = 0; note < CHORD_PIANO_KEY_COUNT; ++note) {
        const float f0 = note_midi_to_frequency(MUSIC_PIANO_MIDI_MIN + note);
        s_ev_raw_score[note] = s_ev_fundamental_score[note];
        for (int harmonic = 2; harmonic <= 8; ++harmonic) {
            const float frequency = f0 * harmonic;
            if (frequency > MUSIC_SPECTRUM_MAX_FREQUENCY_HZ) break;
            s_ev_raw_score[note] += MUSIC_HARMONIC_SUPPORT_WEIGHT *
                                    local_peak(frequency) /
                                    sqrtf((float)harmonic);
        }
    }
    float maximum_ev_fundamental = 0.0f;
    for (int note = 0; note < CHORD_PIANO_KEY_COUNT; ++note) {
        maximum_ev_fundamental = fmaxf(maximum_ev_fundamental,
                                       s_ev_fundamental_score[note]);
    }
    for (int note = 0; note < CHORD_PIANO_KEY_COUNT; ++note) {
        const float note_frequency = note_midi_to_frequency(
            MUSIC_PIANO_MIDI_MIN + note);
        if (s_ev_fundamental_noise_floor[note] <= 0.0f) {
            s_ev_fundamental_noise_floor[note] =
                band_noise_for_frequency(band_noise, note_frequency);
        }
        if (s_ev_fundamental_score[note] <
                s_ev_fundamental_noise_floor[note] *
                    MUSIC_FUNDAMENTAL_NOISE_MULTIPLIER ||
            s_ev_fundamental_score[note] < maximum_ev_fundamental *
                    fundamental_relative_threshold) {
            continue;
        }
        for (int lower = 0; lower < note; ++lower) {
            if (s_ev_harmonic_owned[lower] ||
                s_ev_fundamental_peak_frequency[lower] <= 0.0f ||
                s_ev_fundamental_score[lower] <
                    s_ev_fundamental_noise_floor[lower] *
                        MUSIC_FUNDAMENTAL_NOISE_MULTIPLIER ||
                s_ev_fundamental_score[lower] < maximum_ev_fundamental *
                    fundamental_relative_threshold ||
                s_ev_fundamental_score[lower] <
                    s_ev_fundamental_score[note] *
                        MUSIC_HARMONIC_OWNER_MIN_RELATIVE) {
                continue;
            }
            const float ratio = s_ev_fundamental_peak_frequency[note] /
                                s_ev_fundamental_peak_frequency[lower];
            const int harmonic = (int)lrintf(ratio);
            if (harmonic >= 2 && harmonic <= 8 &&
                fabsf(ratio - harmonic) <= MUSIC_HARMONIC_RATIO_TOLERANCE) {
                s_ev_harmonic_owned[note] = true;
                break;
            }
        }
    }
    float maximum_piano_score = 0.0f;
    const bool strict_low_anchor_available =
        low_anchor_confidence >= MUSIC_LOW_CHORD_YIN_CONFIDENCE &&
        low_anchor_midi >= MUSIC_PIANO_MIDI_MIN &&
        low_anchor_midi <= MUSIC_VIRTUAL_FUNDAMENTAL_MAX_MIDI;
    for (int note = 0; note < CHORD_PIANO_KEY_COUNT; ++note) {
        const int midi = MUSIC_PIANO_MIDI_MIN + note;
        const float f0 = note_midi_to_frequency(midi);
        const bool has_fundamental =
            s_ev_fundamental_score[note] >=
                s_ev_fundamental_noise_floor[note] *
                    MUSIC_FUNDAMENTAL_NOISE_MULTIPLIER &&
            s_ev_fundamental_score[note] >= maximum_ev_fundamental *
                    fundamental_relative_threshold;
        result->key_has_independent_fundamental[note] = has_fundamental;
        if (has_fundamental) {
            float explained = 0.0f;
            for (int lower = 0; lower < note; ++lower) {
                const float lower_f0 = note_midi_to_frequency(
                    MUSIC_PIANO_MIDI_MIN + lower);
                const float ratio = f0 / lower_f0;
                const int harmonic = (int)lrintf(ratio);
                const bool lower_supported =
                    s_ev_fundamental_score[lower] >=
                        s_ev_fundamental_noise_floor[lower] *
                            MUSIC_FUNDAMENTAL_NOISE_MULTIPLIER &&
                    s_ev_fundamental_score[lower] >= maximum_ev_fundamental *
                        fundamental_relative_threshold;
                if (lower_supported && harmonic >= 2 && harmonic <= 8 &&
                    fabsf(ratio - harmonic) <=
                        MUSIC_HARMONIC_RATIO_TOLERANCE) {
                    explained = fmaxf(explained,
                        s_ev_raw_score[lower] / sqrtf((float)harmonic));
                }
            }
            /* Unlike the legacy pitch-class path, do not hard-delete an
             * octave/fifth candidate. Subtract the lower key's predicted
             * partial, leaving energy contributed by a genuinely pressed
             * upper key available to the exact-key tracker. */
            s_ev_piano_score[note] = fmaxf(
                0.0f, s_ev_raw_score[note] - 0.82f * explained);
        }
        if (midi <= MUSIC_VIRTUAL_FUNDAMENTAL_MAX_MIDI &&
            !has_fundamental &&
            (!strict_low_anchor_available || midi == low_anchor_midi)) {
            const float harmonic2_snr = local_peak_snr(2.0f * f0, band_noise);
            const float harmonic3_snr = local_peak_snr(3.0f * f0, band_noise);
            const float harmonic4_snr = local_peak_snr(4.0f * f0, band_noise);
            if (harmonic2_snr >= 2.8f && harmonic3_snr >= 2.5f) {
                const float noise = band_noise_for_frequency(band_noise, f0);
                const float virtual_level = noise *
                    (0.36f * harmonic2_snr + 0.30f * harmonic3_snr +
                     0.14f * fmaxf(0.0f, harmonic4_snr - 1.8f));
                s_ev_piano_score[note] = virtual_level;
                result->key_uses_virtual_fundamental[note] = true;
                s_ev_fundamental_prominence[note] =
                    fminf(harmonic2_snr, harmonic3_snr);
            }
        }
        maximum_piano_score = fmaxf(maximum_piano_score,
                                    s_ev_piano_score[note]);
    }
    if (low_notes != NULL && low_notes->valid) {
        const float reference_score = fmaxf(
            maximum_piano_score,
            fmaxf(noise_floor * MUSIC_FUNDAMENTAL_NOISE_MULTIPLIER * 3.0f,
                  MUSIC_LOW_MATCH_MIN_AMPLITUDE));
        for (uint8_t index = 0; index < low_notes->count; ++index) {
            const int midi = low_notes->midi[index];
            if (midi < MUSIC_PIANO_MIDI_MIN ||
                midi > MUSIC_VIRTUAL_FUNDAMENTAL_MAX_MIDI) {
                continue;
            }
            const int note = midi - MUSIC_PIANO_MIDI_MIN;
            const float confidence = clamp01(low_notes->confidence[index]);
            const float injected = reference_score *
                (0.55f + 0.45f * confidence);
            s_ev_piano_score[note] = fmaxf(s_ev_piano_score[note],
                                           injected);
            s_ev_harmonic_owned[note] = false;
        }
        maximum_piano_score = 0.0f;
        for (int note = 0; note < CHORD_PIANO_KEY_COUNT; ++note) {
            maximum_piano_score = fmaxf(maximum_piano_score,
                                        s_ev_piano_score[note]);
        }
    }
    if (maximum_piano_score > 1.0e-12f) {
        for (int note = 0; note < CHORD_PIANO_KEY_COUNT; ++note) {
            float relative = s_ev_piano_score[note] / maximum_piano_score;
            const float prominence = s_ev_fundamental_prominence[note];
            const float prominence_gate =
                clamp01((prominence - 1.05f) / 1.35f);
            relative *= 0.35f + 0.65f * prominence_gate;
            result->key_salience[note] = clamp01(relative);
            if (result->key_salience[note] >=
                MUSIC_EXACT_KEY_SALIENCE_FLOOR) {
                ++result->supported_note_count;
            }
        }
    }
    for (int note = 0; note < CHORD_PIANO_KEY_COUNT; ++note) {
        result->key_fundamental_prominence[note] =
            s_ev_fundamental_prominence[note];
        if (s_ev_harmonic_owned[note] &&
            result->key_salience[note] <
                EXACT_HARMONIC_RESIDUAL_MIN_SALIENCE) {
            result->key_has_independent_fundamental[note] = false;
        }
    }
    result->too_many_notes = result->supported_note_count >
                             MUSIC_MAX_SIMULTANEOUS_KEYS;
    for (int note = 0; note < CHORD_PIANO_KEY_COUNT; ++note) {
        const float salience = result->key_salience[note];
        if (salience < MUSIC_EXACT_KEY_SALIENCE_FLOOR) continue;
        int insert_at = -1;
        for (int rank = 0; rank < CHORD_EXACT_NOTE_COUNT; ++rank) {
            if (result->exact_midi_notes[rank] < 0 ||
                salience > result->exact_note_confidence[rank]) {
                insert_at = rank;
                break;
            }
        }
        if (insert_at < 0) continue;
        for (int move = CHORD_EXACT_NOTE_COUNT - 1; move > insert_at; --move) {
            result->exact_midi_notes[move] =
                result->exact_midi_notes[move - 1];
            result->exact_note_confidence[move] =
                result->exact_note_confidence[move - 1];
        }
        result->exact_midi_notes[insert_at] = MUSIC_PIANO_MIDI_MIN + note;
        result->exact_note_confidence[insert_at] = salience;
        if (result->exact_note_count < CHORD_EXACT_NOTE_COUNT) {
            ++result->exact_note_count;
        }
    }
    for (int note = 0; note < CHORD_NOTE_COUNT; ++note) {
        if (independent_score[note] <= 0.0f) continue;
        int insert_at = -1;
        for (int rank = 0; rank < CHORD_DEBUG_CANDIDATE_COUNT; ++rank) {
            if (result->debug_candidates[rank].midi < 0 ||
                independent_score[note] >
                    result->debug_candidates[rank].relative_score * maximum_independent_score) {
                insert_at = rank;
                break;
            }
        }
        if (insert_at < 0) continue;
        for (int move = CHORD_DEBUG_CANDIDATE_COUNT - 1; move > insert_at; --move) {
            result->debug_candidates[move] = result->debug_candidates[move - 1];
        }
        chord_candidate_debug_t *candidate = &result->debug_candidates[insert_at];
        candidate->midi = MUSIC_CHORD_MIDI_MIN + note;
        candidate->peak_bin = fundamental_peak_bin[note];
        candidate->peak_frequency_hz = fundamental_peak_frequency[note];
        candidate->relative_score = maximum_independent_score > 1.0e-12f ?
            independent_score[note] / maximum_independent_score : 0.0f;
        candidate->prominence = fundamental_prominence[note];
        candidate->distinct_local_peak = true;
        if (result->debug_candidate_count < CHORD_DEBUG_CANDIDATE_COUNT) {
            ++result->debug_candidate_count;
        }
    }

    float best_note_score[12] = {0};
    int best_midi[12];
    for (int pc = 0; pc < 12; ++pc) best_midi[pc] = -1;
    for (int note = 0; note < CHORD_NOTE_COUNT; ++note) {
        const int pitch_class = (MUSIC_CHORD_MIDI_MIN + note) % 12;
        result->chroma[pitch_class] += independent_score[note];
        if (independent_score[note] > best_note_score[pitch_class]) {
            best_note_score[pitch_class] = independent_score[note];
            best_midi[pitch_class] = MUSIC_CHORD_MIDI_MIN + note;
        }
    }
    float maximum_chroma = 0.0f;
    float total_chroma = 0.0f;
    for (int pc = 0; pc < 12; ++pc) {
        if (result->chroma[pc] > maximum_chroma) maximum_chroma = result->chroma[pc];
        total_chroma += result->chroma[pc];
    }
    if (maximum_chroma <= 1.0e-12f || total_chroma <= 1.0e-12f) return;
    for (int pc = 0; pc < 12; ++pc) {
        result->chroma[pc] /= total_chroma;
        if (result->chroma[pc] >= active_pitch_class_threshold * maximum_chroma / total_chroma) {
            ++result->independent_pitch_class_count;
        }
    }

    float best_confidence = 0.0f;
    int best_root = 0;
    bool best_minor = false;
    for (int root = 0; root < 12; ++root) {
        for (int minor = 0; minor <= 1; ++minor) {
            const int third = (root + (minor ? 3 : 4)) % 12;
            const int fifth = (root + 7) % 12;
            const float root_energy = result->chroma[root];
            const float third_energy = result->chroma[third];
            const float fifth_energy = result->chroma[fifth];
            const float inside = root_energy + third_energy + fifth_energy;
            const float minimum_tone = fminf(root_energy, fminf(third_energy, fifth_energy));
            const float balance = inside > 1.0e-12f ? 3.0f * minimum_tone / inside : 0.0f;
            const float root_evidence = inside > 1.0e-12f ? fminf(1.0f, 2.5f * root_energy / inside) : 0.0f;
            const float confidence = clamp01(0.58f * inside + 0.27f * balance + 0.15f * root_evidence);
            if (confidence > best_confidence) {
                best_confidence = confidence;
                best_root = root;
                best_minor = minor != 0;
            }
        }
    }
    if (result->independent_pitch_class_count >= 3 &&
        (!demo_profile || result->independent_pitch_class_count <=
                              MUSIC_DEMO_INTERVAL_MAX_ACTIVE_CLASSES) &&
        best_confidence >= chord_confidence_threshold) {
        result->valid = true;
        result->kind = best_minor ? CHORD_DETECTION_MINOR : CHORD_DETECTION_MAJOR;
        result->identity = best_root * 2 + (best_minor ? 1 : 0);
        result->root = best_root;
        result->is_minor = best_minor;
        result->confidence = best_confidence;
        result->pitch_class_count = 3;
        result->pitch_classes[0] = best_root;
        result->pitch_classes[1] = (best_root + (best_minor ? 3 : 4)) % 12;
        result->pitch_classes[2] = (best_root + 7) % 12;
        for (int i = 0; i < 3; ++i) result->midi_notes[i] = best_midi[result->pitch_classes[i]];
        snprintf(result->name, sizeof(result->name), "%s:%s", note_pitch_class_name(best_root),
                 best_minor ? "min" : "maj");
        return;
    }

    int top_pc[3] = {-1, -1, -1};
    float top_value[3] = {0};
    for (int pc = 0; pc < 12; ++pc) {
        const float value = result->chroma[pc];
        for (int rank = 0; rank < 3; ++rank) {
            if (value > top_value[rank]) {
                for (int move = 2; move > rank; --move) {
                    top_value[move] = top_value[move - 1];
                    top_pc[move] = top_pc[move - 1];
                }
                top_value[rank] = value;
                top_pc[rank] = pc;
                break;
            }
        }
    }
    if (top_pc[1] < 0) {
        result->confidence = best_confidence;
        return;
    }
    const float pair_energy = top_value[0] + top_value[1];
    const float pair_balance = pair_energy > 1.0e-12f ? 2.0f * top_value[1] / pair_energy : 0.0f;
    const float third_separation = top_value[1] > 1.0e-12f ?
        1.0f - fminf(1.0f, top_value[2] / top_value[1]) : 0.0f;
    const float interval_confidence = clamp01(0.50f * pair_energy +
                                               0.30f * pair_balance +
                                               0.20f * third_separation);
    result->confidence = interval_confidence;
    if (result->independent_pitch_class_count < 2 ||
        result->independent_pitch_class_count > interval_max_active_classes ||
        interval_confidence < interval_confidence_threshold) {
        return;
    }

    const int first_pc = top_pc[0] < top_pc[1] ? top_pc[0] : top_pc[1];
    const int second_pc = top_pc[0] < top_pc[1] ? top_pc[1] : top_pc[0];
    result->valid = true;
    result->kind = CHORD_DETECTION_INTERVAL;
    result->identity = first_pc * 12 + second_pc;
    result->root = first_pc;
    result->pitch_class_count = 2;
    result->pitch_classes[0] = first_pc;
    result->pitch_classes[1] = second_pc;
    result->midi_notes[0] = best_midi[first_pc];
    result->midi_notes[1] = best_midi[second_pc];
    snprintf(result->name, sizeof(result->name), "%s+%s",
             note_pitch_class_name(first_pc), note_pitch_class_name(second_pc));
}

float chord_detector_harmonic_explained_ratio(float fundamental_hz)
{
    if (!s_initialized || fundamental_hz <= 0.0f) return 0.0f;
    const int first_bin = (int)ceilf(MUSIC_MIN_FREQUENCY_HZ * MUSIC_FFT_SIZE / MUSIC_SAMPLE_RATE_HZ);
    const int last_bin = (int)floorf(MUSIC_MAX_FREQUENCY_HZ * MUSIC_FFT_SIZE / MUSIC_SAMPLE_RATE_HZ);
    float total = 0.0f;
    float explained = 0.0f;
    memset(s_harmonic_bin_selected, 0, sizeof(s_harmonic_bin_selected));
    for (int bin = first_bin; bin <= last_bin; ++bin) {
        const float magnitude = s_fused_magnitude[bin];
        total += magnitude * magnitude;
    }
    const float bin_frequency = (float)MUSIC_SAMPLE_RATE_HZ / MUSIC_FFT_SIZE;
    for (int harmonic = 1; harmonic <= 5; ++harmonic) {
        const float frequency = fundamental_hz * harmonic;
        if (frequency > MUSIC_MAX_FREQUENCY_HZ) break;
        const int center = (int)lrintf(frequency / bin_frequency);
        for (int offset = -2; offset <= 2; ++offset) {
            const int bin = center + offset;
            if (bin >= first_bin && bin <= last_bin && !s_harmonic_bin_selected[bin]) {
                const float magnitude = s_fused_magnitude[bin];
                explained += magnitude * magnitude;
                s_harmonic_bin_selected[bin] = true;
            }
        }
    }
    return total > 1.0e-12f ? clamp01(explained / total) : 0.0f;
}
