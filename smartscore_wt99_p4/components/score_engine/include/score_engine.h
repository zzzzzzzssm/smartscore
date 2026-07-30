#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "performance_types.h"
#include "score_data.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCORE_ENGINE_PROFILE_MIDI_STRICT "midi_strict"
#define SCORE_ENGINE_PROFILE_BEGINNER_MONO_V2 "beginner_mono_v2"

/*
 * Deterministic beginner monophonic scoring. midi_strict remains an input
 * alias for older app versions; both sources use the same musical rules.
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
