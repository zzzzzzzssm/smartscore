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
