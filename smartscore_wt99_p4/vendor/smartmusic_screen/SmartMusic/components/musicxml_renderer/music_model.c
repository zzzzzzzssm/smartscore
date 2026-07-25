#include "music_model.h"

#include <string.h>

void music_score_init(music_score_t *score)
{
    if (!score) return;
    memset(score, 0, sizeof(*score));
}

int music_pitch_diatonic_index(const music_pitch_t *pitch)
{
    if (!pitch || pitch->step > 6) return 0;
    return (int)pitch->octave * 7 + (int)pitch->step;
}

int music_pitch_staff_position(const music_pitch_t *pitch,
                               const music_clef_t *clef)
{
    if (!pitch || !clef) return 0;

    int reference_diatonic;
    switch (clef->kind) {
    case MUSIC_CLEF_BASS:
        reference_diatonic = 3 * 7 + 3; /* F3 */
        break;
    case MUSIC_CLEF_ALTO:
        reference_diatonic = 4 * 7;     /* C4 */
        break;
    case MUSIC_CLEF_TREBLE:
    default:
        reference_diatonic = 4 * 7 + 4; /* G4 */
        break;
    }

    int line = clef->line > 0 ? clef->line :
               (clef->kind == MUSIC_CLEF_BASS ? 4 :
                clef->kind == MUSIC_CLEF_ALTO ? 3 : 2);
    int reference_position = (line - 1) * 2;
    return music_pitch_diatonic_index(pitch) - reference_diatonic +
           reference_position - (int)clef->octave_change * 7;
}

music_duration_kind_t music_duration_from_name(const char *name)
{
    if (!name) return MUSIC_DURATION_UNKNOWN;
    if (strcmp(name, "whole") == 0) return MUSIC_DURATION_WHOLE;
    if (strcmp(name, "half") == 0) return MUSIC_DURATION_HALF;
    if (strcmp(name, "quarter") == 0) return MUSIC_DURATION_QUARTER;
    if (strcmp(name, "eighth") == 0) return MUSIC_DURATION_EIGHTH;
    if (strcmp(name, "16th") == 0) return MUSIC_DURATION_16TH;
    if (strcmp(name, "32nd") == 0) return MUSIC_DURATION_32ND;
    return MUSIC_DURATION_UNKNOWN;
}
