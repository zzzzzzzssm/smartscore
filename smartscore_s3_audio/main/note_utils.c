#include "note_utils.h"

#include <math.h>
#include <stdio.h>
#include "music_detector_config.h"

static const char *const s_names[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

static float s_tuning_offset_cents;

static float tuned_reference_a4(void)
{
    return MUSIC_REFERENCE_A4_HZ *
           powf(2.0f, s_tuning_offset_cents / 1200.0f);
}

int note_frequency_to_midi(float frequency_hz)
{
    if (!isfinite(frequency_hz) || frequency_hz <= 0.0f) {
        return -1;
    }
    return (int)lrintf(69.0f + 12.0f *
                       log2f(frequency_hz / tuned_reference_a4()));
}

float note_midi_to_frequency(int midi)
{
    return tuned_reference_a4() *
           powf(2.0f, ((float)midi - 69.0f) / 12.0f);
}

float note_cents_error(float frequency_hz, int midi)
{
    const float reference = note_midi_to_frequency(midi);
    return (frequency_hz > 0.0f && reference > 0.0f) ? 1200.0f * log2f(frequency_hz / reference) : 0.0f;
}

void note_tuning_reset(void)
{
    s_tuning_offset_cents = 0.0f;
}

void note_tuning_observe(float frequency_hz, float confidence)
{
    if (!isfinite(frequency_hz) || frequency_hz <= 0.0f ||
        confidence < MUSIC_TUNING_TRACK_MIN_CONFIDENCE) {
        return;
    }
    const int current_midi = note_frequency_to_midi(frequency_hz);
    const float sample_cents = note_cents_error(frequency_hz,
                                                current_midi);
    if (fabsf(sample_cents) > MUSIC_TUNING_TRACK_MAX_SAMPLE_CENTS) {
        return;
    }
    s_tuning_offset_cents += MUSIC_TUNING_TRACK_ALPHA * sample_cents;
    if (s_tuning_offset_cents > MUSIC_TUNING_TRACK_MAX_OFFSET_CENTS) {
        s_tuning_offset_cents = MUSIC_TUNING_TRACK_MAX_OFFSET_CENTS;
    } else if (s_tuning_offset_cents <
               -MUSIC_TUNING_TRACK_MAX_OFFSET_CENTS) {
        s_tuning_offset_cents = -MUSIC_TUNING_TRACK_MAX_OFFSET_CENTS;
    }
}

float note_tuning_offset_cents(void)
{
    return s_tuning_offset_cents;
}

void note_midi_to_name(int midi, char *output, size_t output_size)
{
    if (output == NULL || output_size == 0) {
        return;
    }
    if (midi < 0 || midi > 127) {
        snprintf(output, output_size, "-");
        return;
    }
    snprintf(output, output_size, "%s%d", s_names[midi % 12], midi / 12 - 1);
}

const char *note_pitch_class_name(int pitch_class)
{
    pitch_class %= 12;
    if (pitch_class < 0) {
        pitch_class += 12;
    }
    return s_names[pitch_class];
}
