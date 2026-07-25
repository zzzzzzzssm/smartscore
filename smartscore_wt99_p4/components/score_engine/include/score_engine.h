#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "performance_types.h"
#include "score_data.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCORE_ENGINE_PROFILE_MIDI_STRICT "midi_strict"

/*
 * MIDI-only phase-one scoring profile. Audio recovery, octave correction and
 * stable-frame merging are deliberately absent from this API.
 */
esp_err_t score_engine_build_midi_result_json(
    const score_document_t *score,
    performance_snapshot_t *performance,
    char **out_json,
    size_t *out_length,
    char *error,
    size_t error_length);

void score_engine_result_free(char *json);

#ifdef __cplusplus
}
#endif
