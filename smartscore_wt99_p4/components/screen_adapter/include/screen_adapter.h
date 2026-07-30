#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "usb_midi.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start display initialization in an independent task. */
esp_err_t screen_adapter_start(void);

/** Fan current USB MIDI events into Creator Mode and live score coloring. */
void screen_adapter_handle_usb_midi_event(const usb_midi_event_t *event);

bool screen_adapter_is_ready(void);

typedef enum {
    SCREEN_VOICE_EVENT_WAKE = 0,
    SCREEN_VOICE_EVENT_TIMEOUT,
    SCREEN_VOICE_EVENT_COMMAND,
} screen_voice_event_type_t;

typedef void (*screen_voice_command_result_cb_t)(uint8_t command_id,
                                                  bool handled,
                                                  void *context);

/** Queue a voice-link event onto the LVGL thread. */
esp_err_t screen_adapter_handle_voice_event(
    screen_voice_event_type_t type,
    uint8_t command_id,
    screen_voice_command_result_cb_t result_callback,
    void *result_context);

typedef enum {
    SCREEN_PREPARATION_IDLE = 0,
    SCREEN_PREPARATION_PREPARED,
    SCREEN_PREPARATION_STARTING,
    SCREEN_PREPARATION_READING,
    SCREEN_PREPARATION_FOLLOWING,
    SCREEN_PREPARATION_ERROR,
} screen_preparation_phase_t;

typedef enum {
    SCREEN_PREPARATION_NUMBERED = 0,
    SCREEN_PREPARATION_STAFF,
} screen_preparation_notation_t;

typedef enum {
    SCREEN_PREPARATION_READ_ONLY = 0,
    SCREEN_PREPARATION_FOLLOW,
} screen_preparation_mode_t;

typedef enum {
    SCREEN_PREPARATION_AUDIO_S3 = 0,
    SCREEN_PREPARATION_USB_MIDI,
} screen_preparation_input_t;

typedef struct {
    bool valid;
    bool screen_ready;
    uint32_t revision;
    screen_preparation_phase_t phase;
    screen_preparation_notation_t notation;
    screen_preparation_mode_t mode;
    screen_preparation_input_t input;
    char filename[64];
    char title[96];
    char key[16];
    int bpm;
    int time_sig_num;
    int time_sig_den;
    size_t note_count;
    esp_err_t last_error;
    char message[96];
} screen_preparation_status_t;

/** Select one SD score and open the shared preparation page. */
esp_err_t screen_adapter_prepare_sd_score(const char *filename);

/** Prepare an already validated uploaded score JSON and open its page. */
esp_err_t screen_adapter_prepare_score_json(const char *json,
                                            size_t json_length,
                                            const char *source_name);

/** Atomically update all preparation options and refresh the screen. */
esp_err_t screen_adapter_update_preparation(
    screen_preparation_notation_t notation,
    screen_preparation_mode_t mode,
    screen_preparation_input_t input);

/** Start the currently prepared score using its canonical options. */
esp_err_t screen_adapter_start_prepared_score(void);

/** Reset a completed result and start the same prepared score again. */
esp_err_t screen_adapter_restart_prepared_score(void);

/**
 * Finish the active practice through the shared screen/scoring flow.
 * Repeated calls while scoring or after the result is ready are harmless.
 */
esp_err_t screen_adapter_complete_practice(void);

void screen_adapter_get_preparation_status(
    screen_preparation_status_t *status);

typedef enum {
    SCREEN_CREATOR_IDLE = 0,
    SCREEN_CREATOR_RECORDING,
    SCREEN_CREATOR_PAUSED,
    SCREEN_CREATOR_SAVING,
    SCREEN_CREATOR_ERROR,
} screen_creator_state_t;

typedef enum {
    SCREEN_CREATOR_STAFF_SINGLE = 1,
    SCREEN_CREATOR_STAFF_GRAND = 2,
} screen_creator_staff_mode_t;

typedef struct {
    int bpm;
    int time_sig_num;
    int time_sig_den;
    screen_creator_staff_mode_t staff_mode;
} screen_creator_config_t;

typedef struct {
    bool available;
    bool active;
    bool waiting_first_note;
    bool usb_midi_connected;
    screen_creator_state_t state;
    screen_creator_config_t config;
    int note_count;
    int measure_count;
    esp_err_t last_error;
    char saved_title[64];
    char saved_filename[64];
    char message[96];
} screen_creator_status_t;

esp_err_t screen_adapter_creator_start(
    const screen_creator_config_t *config);
esp_err_t screen_adapter_creator_pause(void);
esp_err_t screen_adapter_creator_resume(void);
esp_err_t screen_adapter_creator_finish(void);
esp_err_t screen_adapter_creator_cancel(void);
void screen_adapter_creator_get_status(screen_creator_status_t *status);

#ifdef __cplusplus
}
#endif
