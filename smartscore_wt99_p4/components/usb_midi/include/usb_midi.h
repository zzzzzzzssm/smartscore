#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_MIDI_PRODUCT_MAX_LENGTH 64

typedef enum {
    USB_MIDI_EVENT_NOTE_OFF = 0,
    USB_MIDI_EVENT_NOTE_ON,
    USB_MIDI_EVENT_CONNECTED,
    USB_MIDI_EVENT_DISCONNECTED,
    USB_MIDI_EVENT_ERROR,
} usb_midi_event_type_t;

typedef struct {
    uint64_t timestamp_us;
    usb_midi_event_type_t type;
    uint8_t cable;
    uint8_t channel;
    uint8_t midi;
    uint8_t velocity;
} usb_midi_event_t;

typedef struct {
    bool initialized;
    bool connected;
    uint16_t vid;
    uint16_t pid;
    char product[USB_MIDI_PRODUCT_MAX_LENGTH];
    uint32_t dropped_events;
    esp_err_t last_error;
    char error[64];
} usb_midi_status_t;

typedef void (*usb_midi_event_handler_t)(const usb_midi_event_t *event,
                                         void *context);

/** Initialize the P4 USB Host, MIDI class client, event queue and worker. */
esp_err_t usb_midi_init(void);

/** Install the task-context consumer for parsed MIDI and connection events. */
esp_err_t usb_midi_set_event_handler(usb_midi_event_handler_t handler,
                                     void *context);

/** Return a coherent snapshot. Safe to call from HTTP and application tasks. */
void usb_midi_get_status(usb_midi_status_t *out_status);

#ifdef __cplusplus
}
#endif
