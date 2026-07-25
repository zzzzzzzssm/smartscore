#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INPUT_SOURCE_NONE = 0,
    INPUT_SOURCE_USB_MIDI,
    INPUT_SOURCE_AUDIO_S3,
    INPUT_SOURCE_S3_AUDIO = INPUT_SOURCE_AUDIO_S3,
} input_source_t;

typedef struct {
    uint8_t midi;
    uint32_t start_ms;
    uint32_t duration_ms;
    uint8_t velocity;
    uint8_t channel;
    input_source_t source;
} performance_note_t;

typedef struct {
    performance_note_t *notes;
    size_t count;
    uint32_t duration_ms;
    input_source_t input_source;
} performance_snapshot_t;

const char *input_source_name(input_source_t source);
bool input_source_from_name(const char *name, input_source_t *out_source);

#ifdef __cplusplus
}
#endif
