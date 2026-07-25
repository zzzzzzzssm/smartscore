#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_REMOTE_MODE_STOPPED = 0,
    WIFI_REMOTE_MODE_STA,
    WIFI_REMOTE_MODE_AP,
    WIFI_REMOTE_MODE_APSTA,
} wifi_remote_mode_t;

typedef struct {
    bool hosted_transport_ready;
    bool wifi_available;
    bool ble_available;
    uint32_t coprocessor_chip_id;
    char coprocessor_name[20];
    char coprocessor_firmware[32];
} wifi_remote_capabilities_t;

typedef struct {
    bool initialized;
    bool started;
    bool connected;
    wifi_remote_mode_t mode;
    uint8_t disconnect_reason;
    char ip[16];
} wifi_remote_status_t;

/** Initialize esp_netif, the default event loop and synchronization objects. */
esp_err_t wifi_remote_platform_init(void);

/** Start ESP-Hosted exactly once and wait for its transport/init event. */
esp_err_t wifi_remote_hosted_start(void);

/** Initialize esp_wifi through esp_wifi_remote and register event instances. */
esp_err_t wifi_remote_init(void);

/** Stop Wi-Fi, unregister handlers and deinitialize Hosted. */
esp_err_t wifi_remote_deinit(void);

esp_err_t wifi_remote_start_sta(const char *ssid, const char *password);
esp_err_t wifi_remote_retry_sta(void);
esp_err_t wifi_remote_start_ap(const char *ssid, const char *password, uint8_t channel);
esp_err_t wifi_remote_start_apsta(const char *ssid, const char *password, uint8_t channel);
/** Disable the fallback AP while preserving an established STA connection. */
esp_err_t wifi_remote_disable_ap(void);
esp_err_t wifi_remote_stop(void);

esp_err_t wifi_remote_scan(wifi_ap_record_t *records, uint16_t *count);

/**
 * Wait until the STA obtains an IP or disconnects.
 * Returns ESP_OK only after IP_EVENT_STA_GOT_IP.
 */
esp_err_t wifi_remote_wait_for_connection(uint32_t timeout_ms,
                                          char *ip,
                                          size_t ip_size,
                                          uint8_t *disconnect_reason);

wifi_remote_status_t wifi_remote_get_status(void);
wifi_remote_capabilities_t wifi_remote_get_capabilities(void);
void wifi_remote_set_ble_capability(bool available);
esp_netif_t *wifi_remote_get_ap_netif(void);

/* Internal cross-file hooks; not intended for consumers outside this component. */
esp_err_t wifi_remote_events_register(void);
void wifi_remote_events_unregister(void);
void wifi_remote_status_reset(void);
void wifi_remote_status_set_initialized(bool initialized);
void wifi_remote_status_set_started(bool started, wifi_remote_mode_t mode);
void wifi_remote_status_set_connected(const char *ip);
void wifi_remote_status_set_disconnected(uint8_t reason);
void wifi_remote_status_set_hosted(bool ready);
void wifi_remote_status_set_wifi_capability(bool available);
void wifi_remote_status_set_coprocessor(uint32_t chip_id,
                                       const char *name,
                                       const char *firmware);
void wifi_remote_events_clear_connection_bits(void);
void wifi_remote_events_prepare_scan(void);
esp_err_t wifi_remote_events_wait_for_scan(uint32_t timeout_ms,
                                           uint16_t *result_count);

#ifdef __cplusplus
}
#endif
