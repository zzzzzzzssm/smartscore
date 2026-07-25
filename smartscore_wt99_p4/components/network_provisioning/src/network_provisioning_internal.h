#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "network_provisioning_types.h"

typedef enum {
    NETWORK_COMMAND_CONNECT = 0,
    NETWORK_COMMAND_RESET,
    NETWORK_COMMAND_SCAN,
} network_command_type_t;

typedef struct {
    network_command_type_t type;
    int request_id;
    bool from_saved_credentials;
    char ssid[33];
    char password[65];
} network_command_t;

void network_state_machine_init(void);
void network_state_transition(network_state_t state,
                              int request_id,
                              unsigned retry_count,
                              const char *ssid,
                              const char *ip,
                              const char *failure_reason,
                              bool credentials_saved);
network_status_t network_state_snapshot(void);
esp_err_t network_state_add_observer(network_status_observer_t callback,
                                     void *context);
void network_state_remove_observer(network_status_observer_t callback,
                                   void *context);

void network_scan_events_init(void);
esp_err_t network_scan_events_add_observer(network_scan_observer_t callback,
                                           void *context);
void network_scan_events_remove_observer(network_scan_observer_t callback,
                                         void *context);
esp_err_t network_scan_events_publish(const network_scan_event_t *event);

esp_err_t network_credentials_load(char *ssid,
                                   size_t ssid_size,
                                   char *password,
                                   size_t password_size);
esp_err_t network_credentials_save(const char *ssid, const char *password);
esp_err_t network_credentials_clear(void);
