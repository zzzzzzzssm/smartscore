#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 把频率转换为 MIDI 音符编号，例如 440Hz -> 69(A4)。 */
int freq_to_midi(float freq);

/* 把 MIDI 音符编号转换成音名，返回内部静态字符串。 */
const char *midi_to_note_name(int midi);

/* 把 MIDI 音符编号转换成音名，并写入调用者提供的缓冲区。 */
void midi_to_note_name_buf(int midi, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
