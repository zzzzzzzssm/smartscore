#ifndef CREATOR_QUANTIZER_H
#define CREATOR_QUANTIZER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CREATOR_TICKS_PER_QUARTER 480

typedef enum {
    CREATOR_NOTE_WHOLE = 0,
    CREATOR_NOTE_HALF,
    CREATOR_NOTE_QUARTER,
    CREATOR_NOTE_EIGHTH,
    CREATOR_NOTE_SIXTEENTH,
} creator_note_value_t;

typedef struct {
    creator_note_value_t value;
    uint32_t ticks;
    uint32_t nominal_duration_us;
    uint8_t error_percent;
    bool within_tolerance;
} creator_quantized_duration_t;

/* Quantize a measured Note On/Off interval to a conventional note value. */
creator_quantized_duration_t creator_quantize_duration(uint64_t duration_us,
                                                       int bpm,
                                                       uint8_t tolerance_percent);

/* Convert elapsed wall time to score ticks at the configured tempo. */
uint32_t creator_time_us_to_ticks(uint64_t elapsed_us, int bpm);

/* Snap an onset to the sixteenth-note grid used by live engraving. */
uint32_t creator_quantize_onset_tick(uint32_t tick);

#ifdef __cplusplus
}
#endif

#endif
