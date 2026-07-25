#include "pitch_detect.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define YIN_MAX_TAU 256
#define YIN_THRESHOLD 0.22f
#define YIN_PARABOLIC_INTERP_ENABLE 1
#define YIN_OCTAVE_VOTE_ENABLE 1
#define YIN_VOTE_PENALTY_HALF 0.08f
#define YIN_VOTE_PENALTY_QUARTER 0.22f
#define YIN_VOTE_PENALTY_DOUBLE 0.30f

float pitch_detect_autocorr(const int16_t *samples, int n, int sample_rate)
{
    if (samples == NULL || n < 64 || sample_rate <= 0) {
        return 0.0f;
    }

    int min_lag = (int)((float)sample_rate / PITCH_MAX_FREQ_HZ);
    int max_lag = (int)((float)sample_rate / PITCH_MIN_FREQ_HZ);
    if (min_lag < 1) min_lag = 1;
    if (max_lag >= n / 2) max_lag = n / 2 - 1;
    if (max_lag <= min_lag) return 0.0f;

    double mean = 0.0;
    for (int i = 0; i < n; ++i) mean += samples[i];
    mean /= (double)n;

    float best_corr = 0.0f;
    int best_lag = 0;

    for (int lag = min_lag; lag <= max_lag; ++lag) {
        double cross = 0.0, e1 = 0.0, e2 = 0.0;
        for (int i = 0; i < n - lag; ++i) {
            double a = (double)samples[i] - mean;
            double b = (double)samples[i + lag] - mean;
            cross += a * b;
            e1 += a * a;
            e2 += b * b;
        }
        if (e1 <= 1.0 || e2 <= 1.0) continue;
        float corr = (float)(cross / sqrt(e1 * e2));
        if (corr > best_corr) { best_corr = corr; best_lag = lag; }
    }

    if (best_lag == 0 || best_corr < 0.25f) return 0.0f;
    float freq = (float)sample_rate / (float)best_lag;
    if (freq < PITCH_MIN_FREQ_HZ || freq > PITCH_MAX_FREQ_HZ) return 0.0f;
    return freq;
}

float pitch_detect_yin(const int16_t *samples, int n, int sample_rate, float *confidence)
{
    if (confidence) *confidence = 0.0f;
    if (samples == NULL || n < 64 || sample_rate <= 0) return 0.0f;

    int min_tau = (int)((float)sample_rate / PITCH_MAX_FREQ_HZ);
    int max_tau = (int)((float)sample_rate / PITCH_MIN_FREQ_HZ);
    if (min_tau < 2) min_tau = 2;
    if (max_tau >= n / 2) max_tau = n / 2 - 1;
    if (max_tau >= YIN_MAX_TAU) max_tau = YIN_MAX_TAU - 1;
    if (max_tau <= min_tau) return 0.0f;

    double mean = 0.0;
    for (int i = 0; i < n; ++i) mean += samples[i];
    mean /= (double)n;

    static float diff[YIN_MAX_TAU];
    static float cmnd[YIN_MAX_TAU];
    diff[0] = 0.0f; cmnd[0] = 1.0f;

    for (int tau = 1; tau <= max_tau; ++tau) {
        double sum = 0.0;
        for (int i = 0; i < n - tau; ++i) {
            double d = ((double)samples[i] - mean) - ((double)samples[i + tau] - mean);
            sum += d * d;
        }
        diff[tau] = (float)sum;
    }

    float running_sum = 0.0f;
    int best_tau = 0;
    float best_cmnd = 1.0f;

    for (int tau = 1; tau <= max_tau; ++tau) {
        running_sum += diff[tau];
        cmnd[tau] = diff[tau] * (float)tau / (running_sum + 1e-9f);
    }

    for (int tau = min_tau; tau <= max_tau; ++tau) {
        if (cmnd[tau] < YIN_THRESHOLD) {
            while (tau + 1 <= max_tau && cmnd[tau + 1] < cmnd[tau]) tau++;
            best_tau = tau;
            best_cmnd = cmnd[tau];
            break;
        }
        if (cmnd[tau] < best_cmnd) { best_cmnd = cmnd[tau]; best_tau = tau; }
    }

    if (best_tau <= 0 || best_cmnd > 0.55f) return 0.0f;

#if YIN_OCTAVE_VOTE_ENABLE
    struct { int tau; float cmnd; float penalty; float score; } candidates[] = {
        { best_tau, best_cmnd, 0.0f, 0.0f },
        { best_tau / 2, 1.0f, YIN_VOTE_PENALTY_HALF, 0.0f },
        { best_tau / 4, 1.0f, YIN_VOTE_PENALTY_QUARTER, 0.0f },
        { best_tau * 2, 1.0f, YIN_VOTE_PENALTY_DOUBLE, 0.0f },
    };
    int n_candidates = 4;
    for (int c = 0; c < n_candidates; ++c) {
        int t = candidates[c].tau;
        if (t >= min_tau && t <= max_tau) {
            candidates[c].cmnd = cmnd[t];
        } else {
            candidates[c].score = 999.0f;
            continue;
        }
        candidates[c].score = candidates[c].cmnd + candidates[c].penalty;
    }

    int best_idx = 0;
    for (int c = 1; c < n_candidates; ++c) {
        if (candidates[c].score < candidates[best_idx].score) best_idx = c;
    }
    best_tau = candidates[best_idx].tau;
    best_cmnd = candidates[best_idx].cmnd;
#endif

#if YIN_PARABOLIC_INTERP_ENABLE
    if (best_tau > min_tau && best_tau < max_tau) {
        float y1 = cmnd[best_tau - 1];
        float y2 = cmnd[best_tau];
        float y3 = cmnd[best_tau + 1];
        float denom = y1 + y3 - 2.0f * y2;
        if (fabsf(denom) > 1e-9f) {
            float shift = (y1 - y3) / (2.0f * denom);
            float tau_exact = (float)best_tau + shift;
            if (tau_exact > (float)min_tau && tau_exact < (float)max_tau) {
                float freq_exact = (float)sample_rate / tau_exact;
                if (freq_exact >= PITCH_MIN_FREQ_HZ && freq_exact <= PITCH_MAX_FREQ_HZ) {
                    if (confidence) *confidence = 1.0f - best_cmnd;
                    return freq_exact;
                }
            }
        }
    }
#endif

    float freq = (float)sample_rate / (float)best_tau;
    if (freq < PITCH_MIN_FREQ_HZ || freq > PITCH_MAX_FREQ_HZ) return 0.0f;
    if (confidence) *confidence = 1.0f - best_cmnd;
    return freq;
}
