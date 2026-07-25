#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "usb_midi.h"

esp_err_t usb_midi_host_start(void);
void usb_midi_parse_transfer(const uint8_t *data, size_t length);
bool usb_midi_queue_event(const usb_midi_event_t *event);
void usb_midi_publish_connection(bool connected,
                                 uint16_t vid,
                                 uint16_t pid,
                                 const char *product);
void usb_midi_publish_error(esp_err_t error, const char *message);
