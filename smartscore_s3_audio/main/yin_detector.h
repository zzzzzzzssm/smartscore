#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool valid;
    float frequency_hz;
    int midi;
    float cents;
    float confidence;
    char note_name[8];
} yin_result_t;

void yin_detector_init(void);
void yin_detector_analyze(const float *samples, size_t count, yin_result_t *result);
void yin_detector_analyze_range(const float *samples, size_t count,
                                float sample_rate_hz,
                                float minimum_frequency_hz,
                                float maximum_frequency_hz,
                                float threshold,
                                yin_result_t *result);
