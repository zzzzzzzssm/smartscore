#include "note_utils.h"

#include <math.h>
#include <stdio.h>

int freq_to_midi(float freq)
{
    if (freq <= 0.0f) {
        return -1;
    }
    return (int)lrintf(69.0f + 12.0f * log2f(freq / 440.0f));
}

void midi_to_note_name_buf(int midi, char *out, size_t out_len)
{
    static const char *names[] = {
        "C", "C#", "D", "D#", "E", "F",
        "F#", "G", "G#", "A", "A#", "B"
    };

    if (out == NULL || out_len == 0) {
        return;
    }
    if (midi < 0 || midi > 127) {
        snprintf(out, out_len, "-");
        return;
    }

    int octave = (midi / 12) - 1;
    snprintf(out, out_len, "%s%d", names[midi % 12], octave);
}

const char *midi_to_note_name(int midi)
{
    static char ring[4][8];
    static int slot;

    slot = (slot + 1) & 3;
    midi_to_note_name_buf(midi, ring[slot], sizeof(ring[slot]));
    return ring[slot];
}
