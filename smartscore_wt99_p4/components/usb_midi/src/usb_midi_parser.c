#include "usb_midi_internal.h"

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "USB_MIDI_PARSE";

void usb_midi_parse_transfer(const uint8_t *data, size_t length)
{
    if (data == NULL) {
        return;
    }
    if ((length % 4U) != 0U) {
        ESP_LOGW(TAG, "USB-MIDI transfer has %u trailing byte(s)",
                 (unsigned)(length % 4U));
    }

    for (size_t offset = 0; offset + 3U < length; offset += 4U) {
        uint8_t status = data[offset + 1U];
        uint8_t message = status & 0xf0U;
        uint8_t velocity = data[offset + 3U] & 0x7fU;
        usb_midi_event_type_t type;
        if (message == 0x90U && velocity != 0U) {
            type = USB_MIDI_EVENT_NOTE_ON;
        } else if (message == 0x80U ||
                   (message == 0x90U && velocity == 0U)) {
            type = USB_MIDI_EVENT_NOTE_OFF;
        } else {
            continue;
        }

        usb_midi_event_t event = {
            .timestamp_us = (uint64_t)esp_timer_get_time(),
            .type = type,
            .cable = data[offset] >> 4U,
            .channel = status & 0x0fU,
            .midi = data[offset + 2U] & 0x7fU,
            .velocity = velocity,
        };
        usb_midi_queue_event(&event);
    }
}
