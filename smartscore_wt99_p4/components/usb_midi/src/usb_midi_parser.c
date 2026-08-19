#include "usb_midi_internal.h"

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "USB_MIDI_PARSE";
static uint64_t s_last_pitch_bend_us[16];

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
        } else if (message == 0xB0U) {
            type = USB_MIDI_EVENT_CONTROL_CHANGE;
        } else if (message == 0xE0U) {
            type = USB_MIDI_EVENT_PITCH_BEND;
        } else {
            continue;
        }

        uint64_t timestamp_us = (uint64_t)esp_timer_get_time();
        uint8_t channel = status & 0x0fU;
        /* Pitch bend can arrive at USB frame rate. Keep a 100 Hz contour for
         * later gliss semantics without starving note events in the shared
         * product queue. */
        if (type == USB_MIDI_EVENT_PITCH_BEND &&
            timestamp_us - s_last_pitch_bend_us[channel] < 10000U)
            continue;
        if (type == USB_MIDI_EVENT_PITCH_BEND)
            s_last_pitch_bend_us[channel] = timestamp_us;

        usb_midi_event_t event = {
            .timestamp_us = timestamp_us,
            .type = type,
            .cable = data[offset] >> 4U,
            .channel = channel,
            .midi = data[offset + 2U] & 0x7fU,
            .velocity = velocity,
            .controller = data[offset + 2U] & 0x7fU,
            .value = data[offset + 3U] & 0x7fU,
            .pitch_bend = (int16_t)((((uint16_t)data[offset + 3U] & 0x7fU) << 7U) |
                                    ((uint16_t)data[offset + 2U] & 0x7fU)) - 8192,
        };
        usb_midi_queue_event(&event);
    }
}
