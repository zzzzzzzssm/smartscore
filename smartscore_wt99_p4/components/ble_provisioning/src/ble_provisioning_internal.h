#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "host/ble_hs.h"
#include "network_provisioning_types.h"

typedef enum {
    BLE_COMMAND_INVALID = 0,
    BLE_COMMAND_GET_STATUS,
    BLE_COMMAND_SCAN_WIFI,
    BLE_COMMAND_SET_WIFI,
    BLE_COMMAND_RESET_WIFI,
    BLE_COMMAND_GET_DEVICE_INFO,
} ble_command_type_t;

typedef struct {
    int request_id;
    ble_command_type_t command;
    char ssid[33];
    char password[65];
    char error[32];
} ble_decoded_message_t;

typedef void (*ble_message_callback_t)(ble_decoded_message_t *message,
                                       void *context);

void ble_message_codec_reset(void);
esp_err_t ble_message_codec_feed(const uint8_t *data,
                                 size_t length,
                                 ble_message_callback_t callback,
                                 void *context);

esp_err_t ble_gatt_server_init(void);
uint16_t ble_gatt_server_tx_handle(void);

esp_err_t ble_notify_init(void);
void ble_notify_deinit(void);
void ble_notify_set_link(uint16_t connection_handle, bool enabled);
esp_err_t ble_notify_enqueue_json(const char *json);
void ble_notify_network_status(const network_status_t *status, void *context);
esp_err_t ble_notify_scan_event(const network_scan_event_t *event, void *context);
int ble_notify_append_last(struct os_mbuf *output);
