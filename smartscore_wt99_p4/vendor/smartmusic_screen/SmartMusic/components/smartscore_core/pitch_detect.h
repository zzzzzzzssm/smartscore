#pragma once

#include <stdint.h>
#include "note_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PITCH_MIN_FREQ_HZ 80.0f
#define PITCH_MAX_FREQ_HZ 1200.0f

float pitch_detect_autocorr(const int16_t *samples, int n, int sample_rate);

float pitch_detect_yin(const int16_t *samples, int n, int sample_rate, float *confidence);

#ifdef __cplusplus
}
#endif
