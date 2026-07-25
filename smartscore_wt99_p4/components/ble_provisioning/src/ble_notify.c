#include "ble_provisioning_internal.h"

#include <stdio.h>
#include <string.h>

#include "ble_provisioning_protocol.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gatt.h"
#include "os/os_mbuf.h"

typedef struct {
    char json[BLE_PROVISIONING_MAX_MESSAGE_BYTES + 2];
} notify_message_t;

static const char *TAG = "BLE";
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static uint16_t s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_notifications_enabled;
static char s_last_message[BLE_PROVISIONING_MAX_MESSAGE_BYTES + 2] = "{}\n";

static esp_err_t ble_notify_enqueue_json_timeout(const char *json,
                                                 TickType_t wait_ticks);

static void notify_task(void *argument)
{
    (void)argument;
    notify_message_t message;
    while (true) {
        if (xQueueReceive(s_queue, &message, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        size_t length = strlen(message.json);
        if (length == 0) continue;
        if (message.json[length - 1] != '\n' && length + 1 < sizeof(message.json)) {
            message.json[length++] = '\n';
            message.json[length] = '\0';
        }

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        strlcpy(s_last_message, message.json, sizeof(s_last_message));
        uint16_t connection_handle = s_connection_handle;
        bool enabled = s_notifications_enabled;
        xSemaphoreGive(s_mutex);

        if (!enabled || connection_handle == BLE_HS_CONN_HANDLE_NONE) {
            continue;
        }
        uint16_t value_handle = ble_gatt_server_tx_handle();
        for (size_t offset = 0; offset < length; offset += BLE_PROVISIONING_CHUNK_BYTES) {
            size_t chunk_length = length - offset;
            if (chunk_length > BLE_PROVISIONING_CHUNK_BYTES) {
                chunk_length = BLE_PROVISIONING_CHUNK_BYTES;
            }
            struct os_mbuf *packet = ble_hs_mbuf_from_flat(
                message.json + offset, chunk_length);
            if (packet == NULL) {
                ESP_LOGE(TAG, "notification allocation failed");
                break;
            }
            int result = ble_gatts_notify_custom(
                connection_handle, value_handle, packet);
            if (result != 0) {
                ESP_LOGW(TAG, "notification failed, rc=%d", result);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        ESP_LOGD(TAG, "notification message sent");
    }
}

esp_err_t ble_notify_init(void)
{
    if (s_queue != NULL) {
        return ESP_OK;
    }
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    s_queue = xQueueCreate(8, sizeof(notify_message_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(notify_task, "ble_notify", 4096, NULL, 5, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ble_notify_deinit(void)
{
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    if (s_queue != NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
}

void ble_notify_set_link(uint16_t connection_handle, bool enabled)
{
    if (s_mutex == NULL) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_connection_handle = connection_handle;
    s_notifications_enabled = enabled;
    xSemaphoreGive(s_mutex);
}

esp_err_t ble_notify_enqueue_json(const char *json)
{
    return ble_notify_enqueue_json_timeout(json, 0);
}

static esp_err_t ble_notify_enqueue_json_timeout(const char *json,
                                                 TickType_t wait_ticks)
{
    if (s_queue == NULL || json == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t length = strlen(json);
    if (length > BLE_PROVISIONING_MAX_MESSAGE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    notify_message_t message = {0};
    strlcpy(message.json, json, sizeof(message.json));
    return xQueueSend(s_queue, &message, wait_ticks) == pdTRUE
        ? ESP_OK : ESP_ERR_TIMEOUT;
}

void ble_notify_network_status(const network_status_t *status, void *context)
{
    (void)context;
    if (status == NULL) return;
    char json[256];
    if (status->state == NETWORK_STATE_WIFI_CONNECTING) {
        snprintf(json, sizeof(json),
                 "{\"id\":%d,\"event\":\"wifi_state\",\"state\":\"connecting\"}",
                 status->request_id);
    } else if (status->state == NETWORK_STATE_WIFI_CONNECTED) {
        snprintf(json, sizeof(json),
                 "{\"id\":%d,\"event\":\"wifi_state\",\"state\":\"connected\",\"ip\":\"%s\"}",
                 status->request_id, status->ip);
    } else if (status->state == NETWORK_STATE_WIFI_FAILED ||
               status->state == NETWORK_STATE_ERROR) {
        snprintf(json, sizeof(json),
                 "{\"id\":%d,\"event\":\"wifi_state\",\"state\":\"failed\",\"reason\":\"%s\"}",
                 status->request_id,
                 status->failure_reason[0] != '\0'
                    ? status->failure_reason : "UNKNOWN");
    } else if (status->state == NETWORK_STATE_WAITING_CREDENTIALS) {
        snprintf(json, sizeof(json),
                 "{\"id\":%d,\"event\":\"wifi_state\",\"state\":\"waiting_credentials\"}",
                 status->request_id);
    } else {
        return;
    }
    ble_notify_enqueue_json(json);
}

esp_err_t ble_notify_scan_event(const network_scan_event_t *event, void *context)
{
    (void)context;
    if (event == NULL || s_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool link_ready = s_notifications_enabled &&
                      s_connection_handle != BLE_HS_CONN_HANDLE_NONE;
    xSemaphoreGive(s_mutex);
    if (!link_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(root, "id", event->request_id);
    if (event->type == NETWORK_SCAN_EVENT_NETWORK) {
        cJSON_AddStringToObject(root, "event", "wifi_network");
        cJSON_AddStringToObject(root, "status", "wifi_network");
        cJSON_AddStringToObject(root, "ssid", event->ssid);
        cJSON_AddNumberToObject(root, "rssi", event->rssi);
        cJSON_AddNumberToObject(root, "channel", event->channel);
        cJSON_AddBoolToObject(root, "open", event->open);
    } else {
        cJSON_AddStringToObject(root, "event", "wifi_scan");
        if (event->type == NETWORK_SCAN_EVENT_STARTED) {
            cJSON_AddStringToObject(root, "status", "wifi_scan_start");
        } else if (event->type == NETWORK_SCAN_EVENT_COMPLETED) {
            cJSON_AddStringToObject(root, "status", "wifi_scan_done");
            cJSON_AddNumberToObject(root, "count", event->count);
        } else {
            cJSON_AddStringToObject(root, "status", "failed");
            cJSON_AddStringToObject(
                root,
                "reason",
                event->failure_reason[0] != '\0'
                    ? event->failure_reason : "wifi_scan_failed");
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ble_notify_enqueue_json_timeout(
        json, pdMS_TO_TICKS(500));
    cJSON_free(json);
    return err;
}

int ble_notify_append_last(struct os_mbuf *output)
{
    if (output == NULL || s_mutex == NULL) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    char copy[sizeof(s_last_message)];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(copy, s_last_message, sizeof(copy));
    xSemaphoreGive(s_mutex);
    return os_mbuf_append(output, copy, strlen(copy)) == 0
        ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}
