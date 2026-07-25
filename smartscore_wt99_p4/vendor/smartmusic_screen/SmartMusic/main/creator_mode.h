#ifndef CREATOR_MODE_H
#define CREATOR_MODE_H

#include <stdbool.h>
#include <stddef.h>

#include "creator_recorder.h"
#include "esp_err.h"
#include "gui_guider.h"
#include "usb_midi_input.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CREATOR_MODE_STATE_IDLE = 0,
    CREATOR_MODE_STATE_RECORDING,
    CREATOR_MODE_STATE_PAUSED,
    CREATOR_MODE_STATE_SAVING,
    CREATOR_MODE_STATE_ERROR,
} creator_mode_state_t;

typedef struct {
    bool available;
    bool active;
    bool waiting_first_note;
    creator_mode_state_t state;
    creator_recorder_config_t config;
    int note_count;
    int measure_count;
    esp_err_t last_error;
    char saved_title[64];
    char saved_filename[64];
    char message[96];
} creator_mode_status_t;

esp_err_t creator_mode_init(lv_ui *ui);
/* Initialize Creator Mode without binding the main-screen entry button. */
esp_err_t creator_mode_init_detached(lv_ui *ui);
/* Open the preparation page and return to return_screen on Back/Exit. */
void creator_mode_open(lv_obj_t *return_screen);
bool creator_mode_is_active(void);
esp_err_t creator_mode_start(const creator_recorder_config_t *config);
esp_err_t creator_mode_pause(void);
esp_err_t creator_mode_resume(void);
esp_err_t creator_mode_finish(void);
esp_err_t creator_mode_cancel(void);
void creator_mode_get_status(creator_mode_status_t *status);

/* Called by the existing single USB-MIDI queue consumer. */
bool creator_mode_handle_midi_event(const usb_midi_input_event_t *event);

#ifdef __cplusplus
}
#endif

#endif
