#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "performance_types.h"
#include "usb_midi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    input_source_t selected_input;
    input_source_t active_input;
    bool input_locked;
    bool usb_midi_connected;
    uint16_t usb_midi_vid;
    uint16_t usb_midi_pid;
    char usb_midi_product[USB_MIDI_PRODUCT_MAX_LENGTH];
    bool audio_s3_connected;
    bool audio_s3_implemented;
} input_source_status_t;

esp_err_t input_source_manager_init(void);
esp_err_t input_source_manager_select(input_source_t source);
esp_err_t input_source_manager_lock(input_source_t *out_source);
void input_source_manager_unlock(void);
void input_source_manager_get_status(input_source_status_t *out_status);

#ifdef __cplusplus
}
#endif
