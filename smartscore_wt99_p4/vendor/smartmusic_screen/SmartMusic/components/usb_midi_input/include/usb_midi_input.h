#ifndef USB_MIDI_INPUT_H
#define USB_MIDI_INPUT_H

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    USB_MIDI_INPUT_DEVICE_CONNECTED = 0,
    USB_MIDI_INPUT_DEVICE_DISCONNECTED,
    USB_MIDI_INPUT_NOTE_ON,
    USB_MIDI_INPUT_NOTE_OFF,
    USB_MIDI_INPUT_CONTROL_CHANGE,
    USB_MIDI_INPUT_PITCH_BEND,
} usb_midi_input_event_type_t;

typedef struct {
    usb_midi_input_event_type_t type;
    uint64_t timestamp_us;
    uint16_t vid;
    uint16_t pid;
    uint8_t cable;
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
    uint8_t controller;
    uint8_t value;
    int16_t pitch_bend;
    char product[64];
} usb_midi_input_event_t;

/** Start the ESP32-P4 USB Host daemon and USB-MIDI class client tasks. */
esp_err_t usb_midi_input_start(void);

/**
 * Queue carrying hot-plug and note events from the USB task.
 *
 * The queue is created by usb_midi_input_start(). It has a single consumer;
 * that consumer can fan events out to future recording/recognition modules.
 */
QueueHandle_t usb_midi_input_event_queue(void);

#ifdef __cplusplus
}
#endif

#endif
