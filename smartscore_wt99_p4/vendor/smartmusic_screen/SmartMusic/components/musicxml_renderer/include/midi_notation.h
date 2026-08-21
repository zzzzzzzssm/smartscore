#ifndef MIDI_NOTATION_H
#define MIDI_NOTATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "midi_parser.h"
#include "music_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Mapping value used when a source MIDI note did not produce a score event. */
#define MIDI_NOTATION_NO_EVENT UINT16_MAX

/** Stable, inspectable statistics produced by conservative key analysis. */
typedef struct {
    bool accepted;
    bool skipped_forced;
    int8_t fifths;
    bool minor;
    uint8_t confidence_percent;
    uint16_t note_count;
    uint16_t onset_group_count;
    uint16_t completed_measure_count;
    uint16_t c_printed_accidentals;
    uint16_t best_printed_accidentals;
    uint32_t c_notation_cost;
    uint32_t best_notation_cost;
    uint32_t runner_up_notation_cost;
    uint8_t accidental_reduction_percent;
    uint8_t runner_up_margin_percent;
} midi_notation_key_analysis_t;

/**
 * Score-build policy.  The selected key is explicit so a Creator session can
 * lock it at pause time and rebuild later without analysing again.
 */
typedef struct {
    bool infer_key;
    bool tonality_forced;
    int8_t key_fifths;
    bool key_minor;
} midi_notation_build_options_t;

typedef enum {
    MIDI_NOTATION_OK = 0,
    MIDI_NOTATION_INVALID_ARGUMENT,
    MIDI_NOTATION_EMPTY,
    MIDI_NOTATION_CAPACITY,
    MIDI_NOTATION_OUT_OF_MEMORY,
} midi_notation_status_t;

typedef struct {
    int8_t applied_fifths;
    bool applied_minor;
    uint16_t score_event_count;
    uint16_t mapped_note_count;
    uint16_t deduplicated_note_count;
    midi_notation_key_analysis_t key_analysis;
} midi_notation_build_result_t;

/**
 * Analyse the notes without modifying @p midi.  accepted is only true after
 * the minimum sample, accidental reduction and winner-margin gates all pass.
 */
bool midi_notation_analyze_key(const midi_data_t *midi,
                               midi_notation_key_analysis_t *analysis);

/** Fill deterministic build defaults from the MIDI metadata (no inference). */
void midi_notation_build_options_from_midi(
    const midi_data_t *midi, midi_notation_build_options_t *options);

/**
 * Convert MIDI performance data to written notation without changing the
 * source.  midi_to_event may be NULL when the caller does not need hit-test
 * mapping; otherwise its capacity must cover midi->note_count entries.
 * Notes sustaining across barlines are split into tied written segments; a
 * source note maps to its first segment.
 * Semantically identical duplicate pitches map to the same event. Unison
 * notes with different rhythm or connection metadata remain distinct so the
 * normalizer never discards notation meaning.
 */
midi_notation_status_t midi_notation_build_score(
    const midi_data_t *midi,
    const midi_notation_build_options_t *options,
    music_score_t *score,
    uint16_t *midi_to_event,
    size_t midi_to_event_capacity,
    midi_notation_build_result_t *result);

const char *midi_notation_status_name(midi_notation_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_NOTATION_H */
