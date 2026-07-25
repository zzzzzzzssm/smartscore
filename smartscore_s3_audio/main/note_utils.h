#pragma once

#include <stddef.h>

int note_frequency_to_midi(float frequency_hz);
float note_midi_to_frequency(int midi);
float note_cents_error(float frequency_hz, int midi);
void note_tuning_reset(void);
void note_tuning_observe(float frequency_hz, float confidence);
float note_tuning_offset_cents(void);
void note_midi_to_name(int midi, char *output, size_t output_size);
const char *note_pitch_class_name(int pitch_class);
