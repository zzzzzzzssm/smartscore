#ifndef MUSIC_MODEL_H
#define MUSIC_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_MAX_PARTS       4
#define MUSIC_MAX_STAVES      2
#define MUSIC_MAX_MEASURES    96
#define MUSIC_MAX_EVENTS      1024
#define MUSIC_MAX_BEAM_LEVELS 3

typedef float music_sp_t;

typedef enum {
    MUSIC_CLEF_TREBLE = 0,
    MUSIC_CLEF_BASS,
    MUSIC_CLEF_ALTO,
} music_clef_kind_t;

typedef struct {
    music_clef_kind_t kind;
    int8_t line; /* MusicXML line: 1 is the bottom staff line. */
    int8_t octave_change;
} music_clef_t;

typedef enum {
    MUSIC_TIME_NUMERIC = 0,
    MUSIC_TIME_COMMON,
    MUSIC_TIME_CUT,
} music_time_symbol_t;

typedef struct {
    uint8_t beats;
    uint8_t beat_type;
    music_time_symbol_t symbol;
} music_time_signature_t;

typedef struct {
    int8_t fifths; /* -7..+7, circle-of-fifths order. */
    int8_t cancel_fifths; /* Previous fifths cancelled with naturals. */
    bool minor;
} music_key_signature_t;

typedef enum {
    MUSIC_DURATION_UNKNOWN = 0,
    MUSIC_DURATION_WHOLE,
    MUSIC_DURATION_HALF,
    MUSIC_DURATION_QUARTER,
    MUSIC_DURATION_EIGHTH,
    MUSIC_DURATION_16TH,
    MUSIC_DURATION_32ND,
} music_duration_kind_t;

typedef enum {
    MUSIC_ACCIDENTAL_NONE = 0,
    MUSIC_ACCIDENTAL_FLAT,
    MUSIC_ACCIDENTAL_NATURAL,
    MUSIC_ACCIDENTAL_SHARP,
    MUSIC_ACCIDENTAL_DOUBLE_SHARP,
    MUSIC_ACCIDENTAL_DOUBLE_FLAT,
} music_accidental_t;

typedef enum {
    MUSIC_STEM_AUTO = 0,
    MUSIC_STEM_UP,
    MUSIC_STEM_DOWN,
    MUSIC_STEM_NONE,
} music_stem_direction_t;

typedef enum {
    MUSIC_BEAM_NONE = 0,
    MUSIC_BEAM_BEGIN,
    MUSIC_BEAM_CONTINUE,
    MUSIC_BEAM_END,
    MUSIC_BEAM_FORWARD_HOOK,
    MUSIC_BEAM_BACKWARD_HOOK,
} music_beam_state_t;

enum {
    MUSIC_TIE_START = 1u << 0,
    MUSIC_TIE_STOP  = 1u << 1,
};

enum {
    MUSIC_ARTIC_STACCATO = 1u << 0,
    MUSIC_ARTIC_ACCENT   = 1u << 1,
    MUSIC_ARTIC_TENUTO   = 1u << 2,
    MUSIC_ARTIC_FERMATA  = 1u << 3,
};

typedef struct {
    uint8_t step; /* C=0 ... B=6 */
    int8_t alter; /* Semitones: -2 ... +2. */
    int8_t octave;
} music_pitch_t;

typedef struct {
    music_pitch_t pitch;
    music_duration_kind_t type;
    music_accidental_t accidental;
    music_stem_direction_t stem;
    music_beam_state_t beams[MUSIC_MAX_BEAM_LEVELS];
    int32_t duration_divisions;
    uint8_t dots;
    uint8_t voice;
    uint8_t staff;
    uint8_t tie_flags;
    uint8_t articulations;
    uint8_t slur_start;
    uint8_t slur_stop;
    bool chord;
    bool grace;
} music_note_t;

typedef struct {
    music_duration_kind_t type;
    int32_t duration_divisions;
    uint8_t dots;
    uint8_t voice;
    uint8_t staff;
} music_rest_t;

typedef enum {
    MUSIC_BARLINE_SINGLE = 0,
    MUSIC_BARLINE_DOUBLE,
    MUSIC_BARLINE_FINAL,
    MUSIC_BARLINE_REPEAT_START,
    MUSIC_BARLINE_REPEAT_END,
} music_barline_t;

typedef enum {
    MUSIC_EVENT_NOTE = 0,
    MUSIC_EVENT_REST,
    MUSIC_EVENT_CLEF,
    MUSIC_EVENT_KEY_SIGNATURE,
    MUSIC_EVENT_TIME_SIGNATURE,
} music_event_kind_t;

typedef struct {
    music_event_kind_t kind;
    uint16_t part_index;
    uint16_t measure_index;
    int32_t onset_divisions;
    union {
        music_note_t note;
        music_rest_t rest;
        struct { music_clef_t value; uint8_t staff; } clef;
        struct { music_key_signature_t value; uint8_t staff; } key;
        struct { music_time_signature_t value; uint8_t staff; } time;
    } data;
} music_event_t;

typedef struct {
    char number[16];
    uint16_t part_index;
    uint16_t event_start;
    uint16_t event_count;
    int32_t divisions;
    int32_t duration_divisions;
    uint8_t staff_count;
    music_clef_t clefs[MUSIC_MAX_STAVES];
    music_key_signature_t key;
    music_time_signature_t time;
    music_barline_t right_barline;
} music_measure_t;

typedef struct {
    char id[24];
    char name[48];
    uint16_t measure_start;
    uint16_t measure_count;
    uint8_t staff_count;
} music_part_t;

typedef struct {
    char title[96];
    uint16_t part_count;
    uint16_t measure_count;
    uint16_t event_count;
    music_part_t parts[MUSIC_MAX_PARTS];
    music_measure_t measures[MUSIC_MAX_MEASURES];
    music_event_t events[MUSIC_MAX_EVENTS];
} music_score_t;

void music_score_init(music_score_t *score);
int music_pitch_diatonic_index(const music_pitch_t *pitch);
int music_pitch_staff_position(const music_pitch_t *pitch,
                               const music_clef_t *clef);
music_duration_kind_t music_duration_from_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif
