#include "audio_preprocess.h"

#include <math.h>
#include <string.h>
#include "music_detector_config.h"

void audio_preprocess_init(audio_preprocess_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->noise_gate = MUSIC_MIN_RMS;
    for (int band = 0; band < AUDIO_PREPROCESS_BAND_COUNT; ++band) {
        state->band_noise_gate[band] = MUSIC_BAND_MIN_RMS;
    }
}

void audio_preprocess_frame(audio_preprocess_state_t *state, const int16_t *input,
                            float *output, size_t count, audio_frame_metrics_t *metrics)
{
    double input_sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        input_sum += input[i];
    }
    const float mean = count ? (float)(input_sum / ((double)count * 32768.0)) : 0.0f;
    const float rc = 1.0f / (2.0f * (float)M_PI * MUSIC_HIGH_PASS_HZ);
    const float dt = 1.0f / MUSIC_SAMPLE_RATE_HZ;
    const float alpha = rc / (rc + dt);
    const float low_rc = 1.0f / (2.0f * (float)M_PI *
                                  MUSIC_SPECTRUM_LOW_BAND_MAX_HZ);
    const float mid_rc = 1.0f / (2.0f * (float)M_PI *
                                  MUSIC_SPECTRUM_MID_BAND_MAX_HZ);
    const float low_alpha = dt / (low_rc + dt);
    const float mid_alpha = dt / (mid_rc + dt);
    double energy = 0.0;
    double band_energy[AUDIO_PREPROCESS_BAND_COUNT] = {0};
    float peak = 0.0f;
    size_t clip_samples = 0;

    for (size_t i = 0; i < count; ++i) {
        float x = (float)input[i] / 32768.0f - mean;
        for (int stage = 0; stage < 2; ++stage) {
            const float y = alpha * (state->prev_y[stage] + x - state->prev_x[stage]);
            state->prev_x[stage] = x;
            state->prev_y[stage] = y;
            x = y;
        }
        output[i] = x;
        state->band_lowpass[0] += low_alpha *
                                  (x - state->band_lowpass[0]);
        state->band_lowpass[1] += mid_alpha *
                                  (x - state->band_lowpass[1]);
        const float band_sample[AUDIO_PREPROCESS_BAND_COUNT] = {
            state->band_lowpass[0],
            state->band_lowpass[1] - state->band_lowpass[0],
            x - state->band_lowpass[1],
        };
        for (int band = 0; band < AUDIO_PREPROCESS_BAND_COUNT; ++band) {
            band_energy[band] += (double)band_sample[band] *
                                 band_sample[band];
        }
        const float absolute = fabsf(x);
        energy += (double)x * x;
        if (absolute > peak) {
            peak = absolute;
        }
        if (absolute >= MUSIC_CLIP_SAMPLE_THRESHOLD) {
            ++clip_samples;
        }
    }
    metrics->mean = mean;
    metrics->rms = count ? sqrtf((float)(energy / count)) : 0.0f;
    for (int band = 0; band < AUDIO_PREPROCESS_BAND_COUNT; ++band) {
        metrics->band_rms[band] = count
            ? sqrtf((float)(band_energy[band] / count)) : 0.0f;
    }
    metrics->peak = peak;
    metrics->clip_rate = count ? (float)clip_samples / count : 0.0f;
    metrics->clipped = peak >= MUSIC_CLIP_NEAR_THRESHOLD ||
                       metrics->clip_rate > MUSIC_CLIP_RATE_THRESHOLD;

    if (!state->calibrated) {
        state->noise_rms_sum += metrics->rms;
        for (int band = 0; band < AUDIO_PREPROCESS_BAND_COUNT; ++band) {
            state->band_noise_rms_sum[band] += metrics->band_rms[band];
        }
        if (metrics->peak > state->noise_peak) {
            state->noise_peak = metrics->peak;
        }
        ++state->calibration_frames;
    } else if (!metrics->clipped && metrics->rms < state->noise_floor) {
        /* Recover downward if startup calibration included handling noise or
         * keyboard sound. Never adapt upward into a played note. */
        state->noise_floor += MUSIC_NOISE_FLOOR_RECOVERY_ALPHA *
                              (metrics->rms - state->noise_floor);
        const float minimum_floor =
            MUSIC_MIN_RMS / MUSIC_NOISE_GATE_MULTIPLIER;
        if (state->noise_floor < minimum_floor) {
            state->noise_floor = minimum_floor;
        }
        state->noise_gate = fmaxf(
            MUSIC_MIN_RMS,
            state->noise_floor * MUSIC_NOISE_GATE_MULTIPLIER);
    }
    if (state->calibrated && !metrics->clipped) {
        for (int band = 0; band < AUDIO_PREPROCESS_BAND_COUNT; ++band) {
            if (metrics->band_rms[band] >= state->band_noise_floor[band]) {
                continue;
            }
            state->band_noise_floor[band] +=
                MUSIC_NOISE_FLOOR_RECOVERY_ALPHA *
                (metrics->band_rms[band] - state->band_noise_floor[band]);
            const float minimum_floor =
                MUSIC_BAND_MIN_RMS / MUSIC_BAND_NOISE_GATE_MULTIPLIER;
            state->band_noise_floor[band] = fmaxf(
                minimum_floor, state->band_noise_floor[band]);
            state->band_noise_gate[band] = fmaxf(
                MUSIC_BAND_MIN_RMS,
                state->band_noise_floor[band] *
                    MUSIC_BAND_NOISE_GATE_MULTIPLIER);
        }
    }
}

void audio_preprocess_finish_calibration(audio_preprocess_state_t *state)
{
    if (state->calibration_frames > 0) {
        state->noise_floor = state->noise_rms_sum / state->calibration_frames;
        for (int band = 0; band < AUDIO_PREPROCESS_BAND_COUNT; ++band) {
            state->band_noise_floor[band] =
                state->band_noise_rms_sum[band] / state->calibration_frames;
        }
    }
    state->noise_gate = fmaxf(MUSIC_MIN_RMS, state->noise_floor * MUSIC_NOISE_GATE_MULTIPLIER);
    for (int band = 0; band < AUDIO_PREPROCESS_BAND_COUNT; ++band) {
        state->band_noise_gate[band] = fmaxf(
            MUSIC_BAND_MIN_RMS,
            state->band_noise_floor[band] *
                MUSIC_BAND_NOISE_GATE_MULTIPLIER);
    }
    state->calibrated = true;
}

bool audio_preprocess_above_gate(const audio_preprocess_state_t *state,
                                 const audio_frame_metrics_t *metrics)
{
    return state->calibrated && metrics->rms >= state->noise_gate;
}

bool audio_preprocess_band_above_gate(const audio_preprocess_state_t *state,
                                      const audio_frame_metrics_t *metrics,
                                      audio_preprocess_band_t band)
{
    if (state == NULL || metrics == NULL || !state->calibrated ||
        band < AUDIO_PREPROCESS_BAND_LOW ||
        band > AUDIO_PREPROCESS_BAND_HIGH) {
        return false;
    }
    return metrics->band_rms[band] >= state->band_noise_gate[band];
}

bool audio_preprocess_any_band_above_gate(const audio_preprocess_state_t *state,
                                          const audio_frame_metrics_t *metrics)
{
    for (int band = 0; band < AUDIO_PREPROCESS_BAND_COUNT; ++band) {
        if (audio_preprocess_band_above_gate(
                state, metrics, (audio_preprocess_band_t)band)) {
            return true;
        }
    }
    return false;
}

float audio_preprocess_band_snr_db(const audio_preprocess_state_t *state,
                                   const audio_frame_metrics_t *metrics,
                                   audio_preprocess_band_t band)
{
    if (state == NULL || metrics == NULL ||
        band < AUDIO_PREPROCESS_BAND_LOW ||
        band > AUDIO_PREPROCESS_BAND_HIGH) {
        return -120.0f;
    }
    const float reference = fmaxf(
        state->band_noise_floor[band],
        MUSIC_BAND_MIN_RMS / MUSIC_BAND_NOISE_GATE_MULTIPLIER);
    const float ratio = metrics->band_rms[band] /
                        fmaxf(reference, 1.0e-8f);
    return ratio > 1.0e-6f ? 20.0f * log10f(ratio) : -120.0f;
}

void audio_preprocess_rescale_gain(audio_preprocess_state_t *state,
                                   float linear_scale)
{
    if (state == NULL || !isfinite(linear_scale) || linear_scale <= 0.0f) return;
    memset(state->prev_x, 0, sizeof(state->prev_x));
    memset(state->prev_y, 0, sizeof(state->prev_y));
    memset(state->band_lowpass, 0, sizeof(state->band_lowpass));
    state->noise_rms_sum *= linear_scale;
    state->noise_peak *= linear_scale;
    state->noise_floor *= linear_scale;
    for (int band = 0; band < AUDIO_PREPROCESS_BAND_COUNT; ++band) {
        state->band_noise_rms_sum[band] *= linear_scale;
        state->band_noise_floor[band] *= linear_scale;
        if (state->band_noise_floor[band] <
            MUSIC_BAND_MIN_RMS / MUSIC_BAND_NOISE_GATE_MULTIPLIER) {
            state->band_noise_floor[band] =
                MUSIC_BAND_MIN_RMS / MUSIC_BAND_NOISE_GATE_MULTIPLIER;
        }
        state->band_noise_gate[band] = fmaxf(
            MUSIC_BAND_MIN_RMS,
            state->band_noise_floor[band] *
                MUSIC_BAND_NOISE_GATE_MULTIPLIER);
    }
    if (state->noise_floor < MUSIC_MIN_RMS / MUSIC_NOISE_GATE_MULTIPLIER) {
        state->noise_floor = MUSIC_MIN_RMS / MUSIC_NOISE_GATE_MULTIPLIER;
    }
    state->noise_gate = fmaxf(MUSIC_MIN_RMS,
                              state->noise_floor * MUSIC_NOISE_GATE_MULTIPLIER);
}

void audio_preprocess_track_ambient(audio_preprocess_state_t *state,
                                    const audio_frame_metrics_t *metrics)
{
    if (state == NULL || metrics == NULL || !state->calibrated ||
        metrics->clipped) {
        return;
    }
    /* Cap each target step so a sudden non-tonal impact cannot instantly raise
     * the gate. The detector calls this only when neither YIN nor stable local
     * spectral peaks indicate a played note. */
    if (metrics->rms > state->noise_floor) {
        const float limited_target = fminf(
            metrics->rms, state->noise_floor * 1.05f);
        state->noise_floor += MUSIC_DEMO_AMBIENT_TRACK_ALPHA *
                              (limited_target - state->noise_floor);
        state->noise_gate = fmaxf(
            MUSIC_MIN_RMS,
            state->noise_floor * MUSIC_NOISE_GATE_MULTIPLIER);
    }
    for (int band = 0; band < AUDIO_PREPROCESS_BAND_COUNT; ++band) {
        if (metrics->band_rms[band] <= state->band_noise_floor[band]) {
            continue;
        }
        const float limited_band_target = fminf(
            metrics->band_rms[band], state->band_noise_floor[band] * 1.05f);
        state->band_noise_floor[band] += MUSIC_DEMO_AMBIENT_TRACK_ALPHA *
            (limited_band_target - state->band_noise_floor[band]);
        state->band_noise_gate[band] = fmaxf(
            MUSIC_BAND_MIN_RMS,
            state->band_noise_floor[band] *
                MUSIC_BAND_NOISE_GATE_MULTIPLIER);
    }
}
