#ifndef SMUFL_GLYPHS_H
#define SMUFL_GLYPHS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "music_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SMUFL_GLYPH_BRACE = 0,
    SMUFL_GLYPH_REPEAT_DOTS,
    SMUFL_GLYPH_G_CLEF,
    SMUFL_GLYPH_C_CLEF,
    SMUFL_GLYPH_F_CLEF,
    SMUFL_GLYPH_TIME_0,
    SMUFL_GLYPH_TIME_1,
    SMUFL_GLYPH_TIME_2,
    SMUFL_GLYPH_TIME_3,
    SMUFL_GLYPH_TIME_4,
    SMUFL_GLYPH_TIME_5,
    SMUFL_GLYPH_TIME_6,
    SMUFL_GLYPH_TIME_7,
    SMUFL_GLYPH_TIME_8,
    SMUFL_GLYPH_TIME_9,
    SMUFL_GLYPH_TIME_COMMON,
    SMUFL_GLYPH_TIME_CUT,
    SMUFL_GLYPH_NOTEHEAD_WHOLE,
    SMUFL_GLYPH_NOTEHEAD_HALF,
    SMUFL_GLYPH_NOTEHEAD_BLACK,
    SMUFL_GLYPH_AUGMENTATION_DOT,
    SMUFL_GLYPH_FLAG_8_UP,
    SMUFL_GLYPH_FLAG_8_DOWN,
    SMUFL_GLYPH_FLAG_16_UP,
    SMUFL_GLYPH_FLAG_16_DOWN,
    SMUFL_GLYPH_FLAG_32_UP,
    SMUFL_GLYPH_FLAG_32_DOWN,
    SMUFL_GLYPH_ACCIDENTAL_FLAT,
    SMUFL_GLYPH_ACCIDENTAL_NATURAL,
    SMUFL_GLYPH_ACCIDENTAL_SHARP,
    SMUFL_GLYPH_ACCIDENTAL_DOUBLE_SHARP,
    SMUFL_GLYPH_ACCIDENTAL_DOUBLE_FLAT,
    SMUFL_GLYPH_ARTIC_ACCENT_ABOVE,
    SMUFL_GLYPH_ARTIC_ACCENT_BELOW,
    SMUFL_GLYPH_ARTIC_STACCATO_ABOVE,
    SMUFL_GLYPH_ARTIC_STACCATO_BELOW,
    SMUFL_GLYPH_ARTIC_TENUTO_ABOVE,
    SMUFL_GLYPH_ARTIC_TENUTO_BELOW,
    SMUFL_GLYPH_FERMATA_ABOVE,
    SMUFL_GLYPH_FERMATA_BELOW,
    SMUFL_GLYPH_REST_WHOLE,
    SMUFL_GLYPH_REST_HALF,
    SMUFL_GLYPH_REST_QUARTER,
    SMUFL_GLYPH_REST_8,
    SMUFL_GLYPH_REST_16,
    SMUFL_GLYPH_REST_32,
    SMUFL_GLYPH_COUNT,
} smufl_glyph_id_t;

typedef struct {
    smufl_glyph_id_t id;
    const char *name;
    uint32_t codepoint;
} smufl_glyph_info_t;

const smufl_glyph_info_t *smufl_glyph_info(smufl_glyph_id_t id);
smufl_glyph_id_t smufl_clef_glyph(music_clef_kind_t clef);
smufl_glyph_id_t smufl_notehead_glyph(music_duration_kind_t duration);
smufl_glyph_id_t smufl_rest_glyph(music_duration_kind_t duration);
smufl_glyph_id_t smufl_accidental_glyph(music_accidental_t accidental);
smufl_glyph_id_t smufl_flag_glyph(music_duration_kind_t duration,
                                  music_stem_direction_t stem);
bool smufl_codepoint_to_utf8(uint32_t codepoint, char out[5]);

#ifdef __cplusplus
}
#endif

#endif
