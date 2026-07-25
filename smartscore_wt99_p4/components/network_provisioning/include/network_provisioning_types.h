#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define NETWORK_PROVISIONING_SCAN_MAX_RESULTS 15

typedef enum {
    NETWORK_STATE_UNINITIALIZED = 0,
    NETWORK_STATE_HOSTED_STARTING,
    NETWORK_STATE_HOSTED_READY,
    NETWORK_STATE_BLE_ADVERTISING,
    NETWORK_STATE_BLE_CONNECTED,
    NETWORK_STATE_WAITING_CREDENTIALS,
    NETWORK_STATE_WIFI_CONNECTING,
    NETWORK_STATE_WIFI_CONNECTED,
    NETWORK_STATE_WIFI_FAILED,
    NETWORK_STATE_WEB_FALLBACK,
    NETWORK_STATE_ERROR,
} network_state_t;

typedef struct {
    network_state_t state;
    int request_id;
    unsigned retry_count;
    bool credentials_saved;
    char ssid[33];
    char ip[16];
    char failure_reason[32];
} network_status_t;

typedef void (*network_status_observer_t)(const network_status_t *status,
                                          void *context);

typedef enum {
    NETWORK_SCAN_EVENT_STARTED = 0,
    NETWORK_SCAN_EVENT_NETWORK,
    NETWORK_SCAN_EVENT_COMPLETED,
    NETWORK_SCAN_EVENT_FAILED,
} network_scan_event_type_t;

typedef struct {
    network_scan_event_type_t type;
    int request_id;
    uint16_t count;
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool open;
    char failure_reason[32];
} network_scan_event_t;

typedef esp_err_t (*network_scan_observer_t)(const network_scan_event_t *event,
                                             void *context);
