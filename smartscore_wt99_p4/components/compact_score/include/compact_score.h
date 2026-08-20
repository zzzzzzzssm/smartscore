#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COMPACT_SCORE_VERSION 1
#define COMPACT_SCORE_PPQ 24
#define COMPACT_SCORE_MAX_EVENTS 8192U
#define COMPACT_SCORE_NKEY_CAPACITY 4U

typedef enum {
    COMPACT_SCORE_KIND_NUMBERED = 0,
    COMPACT_SCORE_KIND_STAFF,
} compact_score_kind_t;

typedef enum {
    COMPACT_SCORE_OK = 0,
    COMPACT_SCORE_ERR_ARGUMENT,
    COMPACT_SCORE_ERR_NO_MEMORY,
    COMPACT_SCORE_ERR_JSON,
    COMPACT_SCORE_ERR_VERSION,
    COMPACT_SCORE_ERR_KIND,
    COMPACT_SCORE_ERR_META,
    COMPACT_SCORE_ERR_SYSTEMS,
    COMPACT_SCORE_ERR_STAVES,
    COMPACT_SCORE_ERR_BARS,
    COMPACT_SCORE_ERR_EVENT,
    COMPACT_SCORE_ERR_RANGE,
    COMPACT_SCORE_ERR_FLAGS,
    COMPACT_SCORE_ERR_MEASURE_OVERFLOW,
    COMPACT_SCORE_ERR_INCOMPLETE_MEASURE,
    COMPACT_SCORE_ERR_EMPTY,
    COMPACT_SCORE_ERR_CAPACITY,
    COMPACT_SCORE_ERR_UNPLAYABLE,
} compact_score_error_code_t;

typedef struct {
    compact_score_error_code_t code;
    char path[96];
} compact_score_error_t;

typedef struct {
    uint32_t event_index;
    uint32_t start;
    uint32_t absolute_start;
    uint32_t ticks;
    uint32_t source_order;
    uint16_t voice;
    uint8_t flags;
    int8_t degree;
    int8_t octave;
    uint8_t accidental;
    uint8_t *midi;
    size_t midi_count;
    char *lyric;
} compact_score_event_t;

typedef struct {
    uint8_t flags;
    uint8_t *volta;
    size_t volta_count;
    compact_score_event_t *events;
    size_t event_count;
} compact_score_bar_t;

typedef struct {
    char clef;
    compact_score_bar_t *bars;
    size_t bar_count;
} compact_score_staff_t;

typedef struct {
    compact_score_bar_t *bars;
    size_t bar_count;
    compact_score_staff_t *staves;
    size_t staff_count;
} compact_score_system_t;

typedef struct {
    bool has_nkey;
    char nkey[COMPACT_SCORE_NKEY_CAPACITY];
    bool has_key_signature;
    int8_t key_signature;
    bool has_time_signature;
    uint8_t time_signature_numerator;
    uint8_t time_signature_denominator;
    bool has_bpm;
    uint16_t bpm;
    uint8_t ppq;
} compact_score_meta_t;

typedef struct {
    uint8_t version;
    compact_score_kind_t kind;
    compact_score_meta_t meta;
    compact_score_system_t *systems;
    size_t system_count;
    size_t event_count;
    size_t sounding_note_count;
    uint32_t measure_ticks;
    bool reordered;
} compact_score_document_t;

typedef struct {
    uint8_t midi;
    uint8_t flags; /* Source event flags: tie/slur/uncertain. */
    uint16_t staff;
    uint16_t voice;
    uint32_t event_index;
    uint32_t start_ms;
    uint32_t duration_ms;
} compact_score_playback_note_t;

typedef struct {
    uint16_t bpm;
    compact_score_playback_note_t *notes;
    size_t note_count;
} compact_score_playback_t;

compact_score_error_code_t compact_score_parse(
    const char *json,
    size_t length,
    compact_score_document_t **out_document,
    compact_score_error_t *error);

void compact_score_free(compact_score_document_t *document);

compact_score_error_code_t compact_score_build_playback(
    const compact_score_document_t *document,
    uint16_t fallback_bpm,
    compact_score_playback_t *out_playback,
    compact_score_error_t *error);

void compact_score_playback_free(compact_score_playback_t *playback);

const char *compact_score_error_name(compact_score_error_code_t code);

#ifdef __cplusplus
}
#endif
