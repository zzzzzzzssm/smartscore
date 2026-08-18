#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "network_provisioning_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t network_provisioning_init(void);
esp_err_t network_provisioning_start(void);

esp_err_t network_provisioning_submit_credentials(int request_id,
                                                  const char *ssid,
                                                  const char *password);
esp_err_t network_provisioning_submit_reset(int request_id);
esp_err_t network_provisioning_submit_scan(int request_id);

/** Read the already saved credentials for the dedicated voice-S3 handoff. */
esp_err_t network_provisioning_get_saved_credentials(char *ssid,
                                                      size_t ssid_size,
                                                      char *password,
                                                      size_t password_size);

network_status_t network_provisioning_get_status(void);
const char *network_provisioning_state_name(network_state_t state);

esp_err_t network_provisioning_register_observer(network_status_observer_t callback,
                                                 void *context);
void network_provisioning_unregister_observer(network_status_observer_t callback,
                                              void *context);

esp_err_t network_provisioning_register_scan_observer(
    network_scan_observer_t callback,
    void *context);
void network_provisioning_unregister_scan_observer(
    network_scan_observer_t callback,
    void *context);

void network_provisioning_report_hosted_starting(void);
void network_provisioning_report_hosted_ready(void);
void network_provisioning_report_ble_advertising(void);
void network_provisioning_report_ble_connected(void);
void network_provisioning_report_web_fallback(void);

#ifdef __cplusplus
}
#endif
