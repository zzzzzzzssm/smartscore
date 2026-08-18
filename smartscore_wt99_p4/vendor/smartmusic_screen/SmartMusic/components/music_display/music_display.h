#ifndef MUSIC_DISPLAY_H
#define MUSIC_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "midi_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSIC_MSG_MIDI_DATA = 0,
    MUSIC_MSG_MIDI_SNAPSHOT,
    MUSIC_MSG_SWITCH_SCREEN,
    MUSIC_MSG_GESTURE_SWIPE_LEFT,
    MUSIC_MSG_GESTURE_SWIPE_RIGHT,
} music_msg_type_t;

typedef struct {
    music_msg_type_t type;
    union {
        struct { uint8_t *data; size_t len; } midi;
        struct { midi_data_t *parsed; } parsed_midi;
        struct {
            midi_data_t *parsed;
            int notation_type;
            bool allow_empty;
        } snapshot;
    } param;
} music_msg_t;

QueueHandle_t music_display_get_queue(void);
void music_display_start(void);

typedef struct {
    int tempo_bpm;
    int time_sig_num;
    int time_sig_den;
    enum {
        MUSIC_DISPLAY_NOTATION_NUMBERED = 0,
        MUSIC_DISPLAY_NOTATION_STAFF = 1,
    } notation_type;
} music_display_score_options_t;

typedef void (*music_display_note_selection_cb_t)(int note_index,
                                                   int measure_number,
                                                   void *user_data);
void music_display_set_note_selection_callback(
    music_display_note_selection_cb_t callback, void *user_data);

/* Real-time result interface for updates received from the external audio processor. */
void music_display_apply_note_result(int target_index, int expected_midi,
                                      int played_midi, float confidence,
                                      bool pitch_ok, bool rhythm_ok);
/* 1-based inclusive target range; pass 0, 0 to clear. */
void music_display_set_expected_note_group(int first_target_index,
                                           int last_target_index);
void music_display_show_score(const char *result_json);
/* Isolate Creator Mode from audio-recognition result callbacks. */
void music_display_set_creator_active(bool active);
/* Read-only uses the normal paged score view without starting recognition. */
void music_display_set_read_only(bool active);

typedef enum {
    MUSIC_DISPLAY_PAGE_PREVIOUS = -1,
    MUSIC_DISPLAY_PAGE_NEXT = 1,
} music_display_page_direction_t;

typedef enum {
    MUSIC_DISPLAY_PAGE_SOURCE_TOUCH = 0,
    MUSIC_DISPLAY_PAGE_SOURCE_HAND_GESTURE = 1,
    MUSIC_DISPLAY_PAGE_SOURCE_AUTO = 2,
    MUSIC_DISPLAY_PAGE_SOURCE_VOICE = 3,
} music_display_page_source_t;

/* Touch paging is accepted only while practice is paused. */
bool music_display_request_page_turn(
    music_display_page_direction_t direction,
    music_display_page_source_t source);
void music_display_set_practice_navigation_state(bool active, bool paused);

/* Advance by score time without depending on the page-final note. */
void music_display_check_time_page_turn(int64_t score_time_ms);

/* 从 JSON 乐谱生成简谱显示 */
bool music_display_apply_score_json(const char *json);
bool music_display_apply_score_json_with_options(
    const char *json, const music_display_score_options_t *options);


/* Takes ownership of snapshot and queues it for the existing music screen. */
bool music_display_submit_midi_snapshot(
    midi_data_t *snapshot, const music_display_score_options_t *options,
    bool allow_empty);
#ifdef __cplusplus
}
#endif

#endif /* MUSIC_DISPLAY_H */
