#include "smufl_glyphs.h"

static const smufl_glyph_info_t s_glyphs[SMUFL_GLYPH_COUNT] = {
    [SMUFL_GLYPH_BRACE]                   = {SMUFL_GLYPH_BRACE, "brace", 0xE000},
    [SMUFL_GLYPH_REPEAT_DOTS]             = {SMUFL_GLYPH_REPEAT_DOTS, "repeatDots", 0xE044},
    [SMUFL_GLYPH_G_CLEF]                  = {SMUFL_GLYPH_G_CLEF, "gClef", 0xE050},
    [SMUFL_GLYPH_C_CLEF]                  = {SMUFL_GLYPH_C_CLEF, "cClef", 0xE05C},
    [SMUFL_GLYPH_F_CLEF]                  = {SMUFL_GLYPH_F_CLEF, "fClef", 0xE062},
    [SMUFL_GLYPH_TIME_0]                  = {SMUFL_GLYPH_TIME_0, "timeSig0", 0xE080},
    [SMUFL_GLYPH_TIME_1]                  = {SMUFL_GLYPH_TIME_1, "timeSig1", 0xE081},
    [SMUFL_GLYPH_TIME_2]                  = {SMUFL_GLYPH_TIME_2, "timeSig2", 0xE082},
    [SMUFL_GLYPH_TIME_3]                  = {SMUFL_GLYPH_TIME_3, "timeSig3", 0xE083},
    [SMUFL_GLYPH_TIME_4]                  = {SMUFL_GLYPH_TIME_4, "timeSig4", 0xE084},
    [SMUFL_GLYPH_TIME_5]                  = {SMUFL_GLYPH_TIME_5, "timeSig5", 0xE085},
    [SMUFL_GLYPH_TIME_6]                  = {SMUFL_GLYPH_TIME_6, "timeSig6", 0xE086},
    [SMUFL_GLYPH_TIME_7]                  = {SMUFL_GLYPH_TIME_7, "timeSig7", 0xE087},
    [SMUFL_GLYPH_TIME_8]                  = {SMUFL_GLYPH_TIME_8, "timeSig8", 0xE088},
    [SMUFL_GLYPH_TIME_9]                  = {SMUFL_GLYPH_TIME_9, "timeSig9", 0xE089},
    [SMUFL_GLYPH_TIME_COMMON]             = {SMUFL_GLYPH_TIME_COMMON, "timeSigCommon", 0xE08A},
    [SMUFL_GLYPH_TIME_CUT]                = {SMUFL_GLYPH_TIME_CUT, "timeSigCutCommon", 0xE08B},
    [SMUFL_GLYPH_NOTEHEAD_WHOLE]          = {SMUFL_GLYPH_NOTEHEAD_WHOLE, "noteheadWhole", 0xE0A2},
    [SMUFL_GLYPH_NOTEHEAD_HALF]           = {SMUFL_GLYPH_NOTEHEAD_HALF, "noteheadHalf", 0xE0A3},
    [SMUFL_GLYPH_NOTEHEAD_BLACK]          = {SMUFL_GLYPH_NOTEHEAD_BLACK, "noteheadBlack", 0xE0A4},
    [SMUFL_GLYPH_AUGMENTATION_DOT]        = {SMUFL_GLYPH_AUGMENTATION_DOT, "augmentationDot", 0xE1E7},
    [SMUFL_GLYPH_FLAG_8_UP]               = {SMUFL_GLYPH_FLAG_8_UP, "flag8thUp", 0xE240},
    [SMUFL_GLYPH_FLAG_8_DOWN]             = {SMUFL_GLYPH_FLAG_8_DOWN, "flag8thDown", 0xE241},
    [SMUFL_GLYPH_FLAG_16_UP]              = {SMUFL_GLYPH_FLAG_16_UP, "flag16thUp", 0xE242},
    [SMUFL_GLYPH_FLAG_16_DOWN]            = {SMUFL_GLYPH_FLAG_16_DOWN, "flag16thDown", 0xE243},
    [SMUFL_GLYPH_FLAG_32_UP]              = {SMUFL_GLYPH_FLAG_32_UP, "flag32ndUp", 0xE244},
    [SMUFL_GLYPH_FLAG_32_DOWN]            = {SMUFL_GLYPH_FLAG_32_DOWN, "flag32ndDown", 0xE245},
    [SMUFL_GLYPH_ACCIDENTAL_FLAT]         = {SMUFL_GLYPH_ACCIDENTAL_FLAT, "accidentalFlat", 0xE260},
    [SMUFL_GLYPH_ACCIDENTAL_NATURAL]      = {SMUFL_GLYPH_ACCIDENTAL_NATURAL, "accidentalNatural", 0xE261},
    [SMUFL_GLYPH_ACCIDENTAL_SHARP]        = {SMUFL_GLYPH_ACCIDENTAL_SHARP, "accidentalSharp", 0xE262},
    [SMUFL_GLYPH_ACCIDENTAL_DOUBLE_SHARP] = {SMUFL_GLYPH_ACCIDENTAL_DOUBLE_SHARP, "accidentalDoubleSharp", 0xE263},
    [SMUFL_GLYPH_ACCIDENTAL_DOUBLE_FLAT]  = {SMUFL_GLYPH_ACCIDENTAL_DOUBLE_FLAT, "accidentalDoubleFlat", 0xE264},
    [SMUFL_GLYPH_ARTIC_ACCENT_ABOVE]      = {SMUFL_GLYPH_ARTIC_ACCENT_ABOVE, "articAccentAbove", 0xE4A0},
    [SMUFL_GLYPH_ARTIC_ACCENT_BELOW]      = {SMUFL_GLYPH_ARTIC_ACCENT_BELOW, "articAccentBelow", 0xE4A1},
    [SMUFL_GLYPH_ARTIC_STACCATO_ABOVE]    = {SMUFL_GLYPH_ARTIC_STACCATO_ABOVE, "articStaccatoAbove", 0xE4A2},
    [SMUFL_GLYPH_ARTIC_STACCATO_BELOW]    = {SMUFL_GLYPH_ARTIC_STACCATO_BELOW, "articStaccatoBelow", 0xE4A3},
    [SMUFL_GLYPH_ARTIC_TENUTO_ABOVE]      = {SMUFL_GLYPH_ARTIC_TENUTO_ABOVE, "articTenutoAbove", 0xE4A4},
    [SMUFL_GLYPH_ARTIC_TENUTO_BELOW]      = {SMUFL_GLYPH_ARTIC_TENUTO_BELOW, "articTenutoBelow", 0xE4A5},
    [SMUFL_GLYPH_FERMATA_ABOVE]           = {SMUFL_GLYPH_FERMATA_ABOVE, "fermataAbove", 0xE4C0},
    [SMUFL_GLYPH_FERMATA_BELOW]           = {SMUFL_GLYPH_FERMATA_BELOW, "fermataBelow", 0xE4C1},
    [SMUFL_GLYPH_REST_WHOLE]              = {SMUFL_GLYPH_REST_WHOLE, "restWhole", 0xE4E3},
    [SMUFL_GLYPH_REST_HALF]               = {SMUFL_GLYPH_REST_HALF, "restHalf", 0xE4E4},
    [SMUFL_GLYPH_REST_QUARTER]            = {SMUFL_GLYPH_REST_QUARTER, "restQuarter", 0xE4E5},
    [SMUFL_GLYPH_REST_8]                  = {SMUFL_GLYPH_REST_8, "rest8th", 0xE4E6},
    [SMUFL_GLYPH_REST_16]                 = {SMUFL_GLYPH_REST_16, "rest16th", 0xE4E7},
    [SMUFL_GLYPH_REST_32]                 = {SMUFL_GLYPH_REST_32, "rest32nd", 0xE4E8},
};

const smufl_glyph_info_t *smufl_glyph_info(smufl_glyph_id_t id)
{
    if (id < 0 || id >= SMUFL_GLYPH_COUNT) return NULL;
    return &s_glyphs[id];
}

smufl_glyph_id_t smufl_clef_glyph(music_clef_kind_t clef)
{
    if (clef == MUSIC_CLEF_BASS) return SMUFL_GLYPH_F_CLEF;
    if (clef == MUSIC_CLEF_ALTO) return SMUFL_GLYPH_C_CLEF;
    return SMUFL_GLYPH_G_CLEF;
}

smufl_glyph_id_t smufl_notehead_glyph(music_duration_kind_t duration)
{
    if (duration == MUSIC_DURATION_WHOLE) return SMUFL_GLYPH_NOTEHEAD_WHOLE;
    if (duration == MUSIC_DURATION_HALF) return SMUFL_GLYPH_NOTEHEAD_HALF;
    return SMUFL_GLYPH_NOTEHEAD_BLACK;
}

smufl_glyph_id_t smufl_rest_glyph(music_duration_kind_t duration)
{
    switch (duration) {
    case MUSIC_DURATION_WHOLE: return SMUFL_GLYPH_REST_WHOLE;
    case MUSIC_DURATION_HALF: return SMUFL_GLYPH_REST_HALF;
    case MUSIC_DURATION_EIGHTH: return SMUFL_GLYPH_REST_8;
    case MUSIC_DURATION_16TH: return SMUFL_GLYPH_REST_16;
    case MUSIC_DURATION_32ND: return SMUFL_GLYPH_REST_32;
    default: return SMUFL_GLYPH_REST_QUARTER;
    }
}

smufl_glyph_id_t smufl_accidental_glyph(music_accidental_t accidental)
{
    switch (accidental) {
    case MUSIC_ACCIDENTAL_FLAT: return SMUFL_GLYPH_ACCIDENTAL_FLAT;
    case MUSIC_ACCIDENTAL_NATURAL: return SMUFL_GLYPH_ACCIDENTAL_NATURAL;
    case MUSIC_ACCIDENTAL_SHARP: return SMUFL_GLYPH_ACCIDENTAL_SHARP;
    case MUSIC_ACCIDENTAL_DOUBLE_SHARP: return SMUFL_GLYPH_ACCIDENTAL_DOUBLE_SHARP;
    case MUSIC_ACCIDENTAL_DOUBLE_FLAT: return SMUFL_GLYPH_ACCIDENTAL_DOUBLE_FLAT;
    default: return SMUFL_GLYPH_COUNT;
    }
}

smufl_glyph_id_t smufl_flag_glyph(music_duration_kind_t duration,
                                  music_stem_direction_t stem)
{
    bool down = stem == MUSIC_STEM_DOWN;
    if (duration == MUSIC_DURATION_EIGHTH)
        return down ? SMUFL_GLYPH_FLAG_8_DOWN : SMUFL_GLYPH_FLAG_8_UP;
    if (duration == MUSIC_DURATION_16TH)
        return down ? SMUFL_GLYPH_FLAG_16_DOWN : SMUFL_GLYPH_FLAG_16_UP;
    if (duration == MUSIC_DURATION_32ND)
        return down ? SMUFL_GLYPH_FLAG_32_DOWN : SMUFL_GLYPH_FLAG_32_UP;
    return SMUFL_GLYPH_COUNT;
}

bool smufl_codepoint_to_utf8(uint32_t cp, char out[5])
{
    if (!out || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        out[1] = '\0';
    } else if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = '\0';
    } else if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = '\0';
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        out[4] = '\0';
    }
    return true;
}
