#ifndef SCORE_UI_FLOW_H
#define SCORE_UI_FLOW_H

#include <stdbool.h>

#include "gui_guider.h"
#include "score_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCORE_INPUT_MICROPHONE = 0,
    SCORE_INPUT_USB_MIDI = 1,
} score_input_source_t;

typedef enum {
    SCORE_NOTATION_NUMBERED = 0,
    SCORE_NOTATION_STAFF = 1,
} score_notation_type_t;

typedef struct {
    int score_bpm;
    int score_time_sig_num;
    int score_time_sig_den;
    int metronome_bpm;
    int metronome_time_sig_num;
    int metronome_time_sig_den;
    bool metronome_enabled;
    score_input_source_t input_source;
    score_notation_type_t notation_type;
    bool read_only;
} score_practice_options_t;

typedef bool (*score_ui_start_cb_t)(const char *filename,
                                    const score_practice_options_t *options,
                                    void *user_data);

typedef bool (*score_ui_prepare_changed_cb_t)(
    const score_info_t *score,
    const score_practice_options_t *options,
    void *user_data);

typedef bool (*score_ui_choose_back_override_cb_t)(void *user_data);

void score_ui_flow_set_start_callback(score_ui_start_cb_t callback,
                                      void *user_data);
void score_ui_flow_set_prepare_changed_callback(
    score_ui_prepare_changed_cb_t callback,
    void *user_data);
void score_ui_flow_set_choose_back_override(
    score_ui_choose_back_override_cb_t callback,
    void *user_data);
/* Suppress score-directory population while the shared choose screen is
 * temporarily used by another mode (for example WAV playback). */
void score_ui_flow_set_choose_population_enabled(bool enabled);

void score_ui_flow_open_choose(lv_ui *ui);
void score_ui_flow_init_choose(lv_ui *ui);
bool score_ui_flow_open_preparation(lv_ui *ui,
                                    const score_info_t *score,
                                    bool read_only);
/* Return from preview to the currently selected score's preparation page. */
bool score_ui_flow_return_to_preparation(lv_ui *ui);
bool score_ui_flow_apply_preparation_options(
    score_input_source_t input_source,
    score_notation_type_t notation_type,
    bool read_only);
bool score_ui_flow_copy_preparation(score_info_t *score,
                                    score_practice_options_t *options);
bool score_ui_flow_start_prepared(void);

/* Replace the preparation-page status from a start callback. */
void score_ui_flow_set_prepare_status(const char *text);

#ifdef __cplusplus
}
#endif

#endif
