#include "yin_detector.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "music_detector_config.h"
#include "note_utils.h"

#define YIN_MAX_TAU_CAPACITY ((MUSIC_SAMPLE_RATE_HZ + MUSIC_MIN_FREQUENCY_HZ_INTEGER - 1) / \
                              MUSIC_MIN_FREQUENCY_HZ_INTEGER + 2)

static float s_difference[YIN_MAX_TAU_CAPACITY];
static float s_cmnd[YIN_MAX_TAU_CAPACITY];

void yin_detector_init(void)
{
    memset(s_difference, 0, sizeof(s_difference));
    memset(s_cmnd, 0, sizeof(s_cmnd));
}

static int local_minimum(int tau, int min_tau, int max_tau)
{
    while (tau > min_tau && s_cmnd[tau - 1] < s_cmnd[tau]) {
        --tau;
    }
    while (tau < max_tau && s_cmnd[tau + 1] < s_cmnd[tau]) {
        ++tau;
    }
    return tau;
}

void yin_detector_analyze_range(const float *samples, size_t count,
                                float sample_rate_hz, float minimum_frequency_hz,
                                float maximum_frequency_hz, yin_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->midi = -1;
    if (samples == NULL || count < 8 || sample_rate_hz <= 0.0f ||
        minimum_frequency_hz <= 0.0f ||
        maximum_frequency_hz <= minimum_frequency_hz) {
        return;
    }
    int min_tau = (int)floorf(sample_rate_hz / maximum_frequency_hz);
    int max_tau = (int)ceilf(sample_rate_hz / minimum_frequency_hz);
    if (min_tau < 2) min_tau = 2;
    if (max_tau >= (int)count / 2) max_tau = (int)count / 2 - 1;
    if (max_tau >= YIN_MAX_TAU_CAPACITY) max_tau = YIN_MAX_TAU_CAPACITY - 1;

    s_difference[0] = 0.0f;
    s_cmnd[0] = 1.0f;
    for (int tau = 1; tau <= max_tau; ++tau) {
        const size_t limit = count - (size_t)tau;
        size_t i = 0;
        float sum0 = 0.0f;
        float sum1 = 0.0f;
        float sum2 = 0.0f;
        float sum3 = 0.0f;
        /* Four independent accumulators keep the ESP32-S3 single-precision FPU busy
         * without changing YIN's window, tau range, or difference function. */
        for (; i + 3 < limit; i += 4) {
            const float delta0 = samples[i] - samples[i + tau];
            const float delta1 = samples[i + 1] - samples[i + tau + 1];
            const float delta2 = samples[i + 2] - samples[i + tau + 2];
            const float delta3 = samples[i + 3] - samples[i + tau + 3];
            sum0 += delta0 * delta0;
            sum1 += delta1 * delta1;
            sum2 += delta2 * delta2;
            sum3 += delta3 * delta3;
        }
        float sum = (sum0 + sum1) + (sum2 + sum3);
        for (; i < limit; ++i) {
            const float delta = samples[i] - samples[i + tau];
            sum += delta * delta;
        }
        s_difference[tau] = sum;
    }
    float running_sum = 0.0f;
    for (int tau = 1; tau <= max_tau; ++tau) {
        running_sum += s_difference[tau];
        s_cmnd[tau] = running_sum > 0.0f ? s_difference[tau] * tau / running_sum : 1.0f;
    }

    int selected_tau = 0;
    float minimum = 1.0f;
    int global_min_tau = min_tau;
    for (int tau = min_tau; tau <= max_tau; ++tau) {
        if (s_cmnd[tau] < minimum) {
            minimum = s_cmnd[tau];
            global_min_tau = tau;
        }
        if (s_cmnd[tau] < MUSIC_YIN_THRESHOLD) {
            selected_tau = local_minimum(tau, min_tau, max_tau);
            break;
        }
    }
    if (selected_tau == 0) {
        selected_tau = global_min_tau;
    }

    /* Standard YIN already selects the first acceptable local minimum. Do not
     * prefer 2*tau: every periodic tone also has a minimum there, which caused
     * stable A4 input to alternate between A4 and the false subharmonic A3. */
    const float confidence = fmaxf(0.0f, fminf(1.0f, 1.0f - s_cmnd[selected_tau]));
    if (confidence < 0.20f) {
        return;
    }

    float tau_exact = (float)selected_tau;
    if (selected_tau > min_tau && selected_tau < max_tau) {
        const float left = s_cmnd[selected_tau - 1];
        const float center = s_cmnd[selected_tau];
        const float right = s_cmnd[selected_tau + 1];
        const float denominator = left - 2.0f * center + right;
        if (fabsf(denominator) > 1.0e-12f) {
            const float shift = 0.5f * (left - right) / denominator;
            if (fabsf(shift) <= 1.0f) tau_exact += shift;
        }
    }
    const float frequency = sample_rate_hz / tau_exact;
    if (frequency < minimum_frequency_hz || frequency > maximum_frequency_hz) {
        return;
    }
    result->midi = note_frequency_to_midi(frequency);
    if (result->midi < 0 || result->midi > 127) {
        result->midi = -1;
        return;
    }
    result->valid = true;
    result->frequency_hz = frequency;
    result->confidence = confidence;
    result->cents = note_cents_error(frequency, result->midi);
    note_midi_to_name(result->midi, result->note_name, sizeof(result->note_name));
}

void yin_detector_analyze(const float *samples, size_t count, yin_result_t *result)
{
    yin_detector_analyze_range(samples, count, MUSIC_SAMPLE_RATE_HZ,
                               MUSIC_MIN_FREQUENCY_HZ,
                               MUSIC_MAX_FREQUENCY_HZ, result);
}
