#include "creator_quantizer.h"

#include <limits.h>
#include <stddef.h>

typedef struct {
    creator_note_value_t value;
    uint16_t quarter_units;
    uint32_t ticks;
} duration_candidate_t;

static const duration_candidate_t s_candidates[] = {
    {CREATOR_NOTE_WHOLE,     16, CREATOR_TICKS_PER_QUARTER * 4},
    {CREATOR_NOTE_HALF,       8, CREATOR_TICKS_PER_QUARTER * 2},
    {CREATOR_NOTE_QUARTER,    4, CREATOR_TICKS_PER_QUARTER},
    {CREATOR_NOTE_EIGHTH,     2, CREATOR_TICKS_PER_QUARTER / 2},
    {CREATOR_NOTE_SIXTEENTH,  1, CREATOR_TICKS_PER_QUARTER / 4},
};

uint32_t creator_time_us_to_ticks(uint64_t elapsed_us, int bpm)
{
    if (bpm < 1) bpm = 120;
    uint64_t numerator = elapsed_us * (uint64_t)bpm *
                         CREATOR_TICKS_PER_QUARTER;
    return (uint32_t)((numerator + 30000000ULL) / 60000000ULL);
}

uint32_t creator_quantize_onset_tick(uint32_t tick)
{
    const uint32_t grid = CREATOR_TICKS_PER_QUARTER / 4;
    return ((tick + grid / 2) / grid) * grid;
}

creator_quantized_duration_t creator_quantize_duration(uint64_t duration_us,
                                                       int bpm,
                                                       uint8_t tolerance_percent)
{
    if (bpm < 1) bpm = 120;
    if (tolerance_percent > 100) tolerance_percent = 100;

    creator_quantized_duration_t result = {
        .value = CREATOR_NOTE_QUARTER,
        .ticks = CREATOR_TICKS_PER_QUARTER,
        .nominal_duration_us = (uint32_t)(60000000ULL / (uint32_t)bpm),
        .error_percent = 100,
        .within_tolerance = false,
    };
    uint64_t best_error = UINT64_MAX;

    const uint64_t quarter_us = 60000000ULL / (uint32_t)bpm;
    for (size_t i = 0; i < sizeof(s_candidates) / sizeof(s_candidates[0]); ++i) {
        uint64_t nominal = quarter_us * s_candidates[i].quarter_units / 4;
        uint64_t error = duration_us > nominal ?
                         duration_us - nominal : nominal - duration_us;
        if (error >= best_error) continue;
        best_error = error;
        result.value = s_candidates[i].value;
        result.ticks = s_candidates[i].ticks;
        result.nominal_duration_us = (uint32_t)nominal;
        uint64_t percent = nominal ? (error * 100 + nominal / 2) / nominal : 100;
        result.error_percent = percent > 255 ? 255 : (uint8_t)percent;
    }
    result.within_tolerance = result.error_percent <= tolerance_percent;
    return result;
}
