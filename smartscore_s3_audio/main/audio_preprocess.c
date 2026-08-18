#include "audio_preprocess.h"

#include <math.h>
#include <string.h>
#include "music_detector_config.h"

void audio_preprocess_init(audio_preprocess_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->noise_gate = MUSIC_MIN_RMS;
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
    double energy = 0.0;
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
    metrics->peak = peak;
    metrics->clip_rate = count ? (float)clip_samples / count : 0.0f;
    metrics->clipped = peak >= MUSIC_CLIP_NEAR_THRESHOLD ||
                       metrics->clip_rate > MUSIC_CLIP_RATE_THRESHOLD;

    if (!state->calibrated) {
        state->noise_rms_sum += metrics->rms;
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
}

void audio_preprocess_finish_calibration(audio_preprocess_state_t *state)
{
    if (state->calibration_frames > 0) {
        state->noise_floor = state->noise_rms_sum / state->calibration_frames;
    }
    state->noise_gate = fmaxf(MUSIC_MIN_RMS, state->noise_floor * MUSIC_NOISE_GATE_MULTIPLIER);
    state->calibrated = true;
}

bool audio_preprocess_above_gate(const audio_preprocess_state_t *state,
                                 const audio_frame_metrics_t *metrics)
{
    return state->calibrated && metrics->rms >= state->noise_gate;
}

void audio_preprocess_rescale_gain(audio_preprocess_state_t *state,
                                   float linear_scale)
{
    if (state == NULL || !isfinite(linear_scale) || linear_scale <= 0.0f) return;
    memset(state->prev_x, 0, sizeof(state->prev_x));
    memset(state->prev_y, 0, sizeof(state->prev_y));
    state->noise_rms_sum *= linear_scale;
    state->noise_peak *= linear_scale;
    state->noise_floor *= linear_scale;
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
        metrics->clipped || metrics->rms <= state->noise_floor) {
        return;
    }
    /* Cap each target step so a sudden non-tonal impact cannot instantly raise
     * the gate. The detector calls this only when neither YIN nor stable local
     * spectral peaks indicate a played note. */
    const float limited_target = fminf(metrics->rms, state->noise_floor * 1.05f);
    state->noise_floor += MUSIC_DEMO_AMBIENT_TRACK_ALPHA *
                          (limited_target - state->noise_floor);
    state->noise_gate = fmaxf(MUSIC_MIN_RMS,
                              state->noise_floor * MUSIC_NOISE_GATE_MULTIPLIER);
}
