#include "wifi_remote.h"

#include <stdio.h>

#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "WIFI";

static EventGroupHandle_t s_connection_events;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static uint32_t s_scan_status;
static uint16_t s_scan_result_count;

#define WIFI_GOT_IP_BIT BIT0
#define WIFI_DISCONNECTED_BIT BIT1
#define WIFI_SCAN_DONE_BIT BIT2

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_base;

    if (event_id == WIFI_EVENT_STA_START) {
        /*
         * Starting STA is also required for an active scan.  Connection is
         * therefore explicit in wifi_remote_start_sta(), after valid
         * credentials have been installed, rather than automatic here.
         */
        ESP_LOGI(TAG, "STA started");
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        uint8_t reason = 0;
        if (event_data != NULL) {
            const wifi_event_sta_disconnected_t *event = event_data;
            reason = event->reason;
        }
        wifi_remote_status_set_disconnected(reason);
        xEventGroupClearBits(s_connection_events, WIFI_GOT_IP_BIT);
        xEventGroupSetBits(s_connection_events, WIFI_DISCONNECTED_BIT);
        ESP_LOGW(TAG, "disconnected, reason=%u", reason);
        /* Retrying is intentionally owned by the provisioning task. */
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        uint32_t status = 1;
        uint16_t result_count = 0;
        if (event_data != NULL) {
            const wifi_event_sta_scan_done_t *event = event_data;
            status = event->status;
            result_count = event->number;
        }
        s_scan_status = status;
        s_scan_result_count = result_count;
        xEventGroupSetBits(s_connection_events, WIFI_SCAN_DONE_BIT);
        ESP_LOGI(TAG, "scan done, status=%lu results=%u",
                 (unsigned long)status, result_count);
    }
}

static void ip_event_handler(void *arg,
                             esp_event_base_t event_base,
                             int32_t event_id,
                             void *event_data)
{
    (void)arg;
    (void)event_base;
    if (event_id != IP_EVENT_STA_GOT_IP || event_data == NULL) {
        return;
    }

    const ip_event_got_ip_t *event = event_data;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
    wifi_remote_status_set_connected(ip);
    xEventGroupClearBits(s_connection_events, WIFI_DISCONNECTED_BIT);
    xEventGroupSetBits(s_connection_events, WIFI_GOT_IP_BIT);
    ESP_LOGI(TAG, "got IP: %s", ip);
}

esp_err_t wifi_remote_events_register(void)
{
    if (s_connection_events != NULL) {
        return ESP_OK;
    }
    s_connection_events = xEventGroupCreate();
    if (s_connection_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &s_wifi_handler);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL, &s_ip_handler);
    }
    if (err != ESP_OK) {
        if (s_wifi_handler != NULL) {
            esp_event_handler_instance_unregister(
                WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
            s_wifi_handler = NULL;
        }
        vEventGroupDelete(s_connection_events);
        s_connection_events = NULL;
    }
    return err;
}

void wifi_remote_events_unregister(void)
{
    if (s_wifi_handler != NULL) {
        esp_event_handler_instance_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        s_wifi_handler = NULL;
    }
    if (s_ip_handler != NULL) {
        esp_event_handler_instance_unregister(
            IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
        s_ip_handler = NULL;
    }
    if (s_connection_events != NULL) {
        vEventGroupDelete(s_connection_events);
        s_connection_events = NULL;
    }
}

void wifi_remote_events_clear_connection_bits(void)
{
    if (s_connection_events != NULL) {
        xEventGroupClearBits(
            s_connection_events, WIFI_GOT_IP_BIT | WIFI_DISCONNECTED_BIT);
    }
}

void wifi_remote_events_prepare_scan(void)
{
    s_scan_status = 1;
    s_scan_result_count = 0;
    if (s_connection_events != NULL) {
        xEventGroupClearBits(s_connection_events, WIFI_SCAN_DONE_BIT);
    }
}

esp_err_t wifi_remote_events_wait_for_scan(uint32_t timeout_ms,
                                           uint16_t *result_count)
{
    if (s_connection_events == NULL || result_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_connection_events,
        WIFI_SCAN_DONE_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));
    if ((bits & WIFI_SCAN_DONE_BIT) == 0) {
        return ESP_ERR_TIMEOUT;
    }

    *result_count = s_scan_result_count;
    return s_scan_status == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t wifi_remote_wait_for_connection(uint32_t timeout_ms,
                                          char *ip,
                                          size_t ip_size,
                                          uint8_t *disconnect_reason)
{
    if (s_connection_events == NULL || ip == NULL || ip_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_connection_events,
        WIFI_GOT_IP_BIT | WIFI_DISCONNECTED_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    wifi_remote_status_t status = wifi_remote_get_status();
    if (disconnect_reason != NULL) {
        *disconnect_reason = status.disconnect_reason;
    }
    if ((bits & WIFI_GOT_IP_BIT) != 0 && status.connected) {
        strlcpy(ip, status.ip, ip_size);
        return ESP_OK;
    }
    if ((bits & WIFI_DISCONNECTED_BIT) != 0) {
        return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}
