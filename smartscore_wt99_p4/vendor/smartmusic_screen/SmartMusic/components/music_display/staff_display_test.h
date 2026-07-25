/*
 * staff_display_test.h
 *
 * MusicXML -> music model -> staff-space layout -> Leland SMuFL/LVGL test.
 */

#ifndef STAFF_DISPLAY_TEST_H
#define STAFF_DISPLAY_TEST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 显示 MusicXML/Leland 排版测试界面
 *
 * 测试解析、标准化模型、staff-space 布局、SMuFL 字形解析、beam、
 * tie/slur、休止符、附点、表情记号和多系统换行。
 */
void staff_display_test_show(void);

/**
 * @brief Override all rendered elements belonging to one MusicXML event.
 *
 * This is the reserved integration point for future performance-recognition
 * feedback. color_rgb uses RGB888 (for example 0x16A34A green or 0xDC2626
 * red). The function is safe to call from a non-LVGL task.
 */
bool staff_display_test_set_event_color(int event_index, uint32_t color_rgb);

/** Restore one MusicXML event to the normal notation color. */
bool staff_display_test_clear_event_color(int event_index);

/** Clear the test score and switch the page to live USB-MIDI input mode. */
void staff_display_test_midi_connected(void);

/** Mark the current live input device disconnected without deleting its notes. */
void staff_display_test_midi_disconnected(void);

/** Append one received MIDI Note On as the next quarter note on the piano staff. */
void staff_display_test_midi_note_on(uint8_t channel, uint8_t note,
                                     uint8_t velocity);

#ifdef __cplusplus
}
#endif

#endif /* STAFF_DISPLAY_TEST_H */
