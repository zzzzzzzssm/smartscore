#include "ble_provisioning_internal.h"

#include <stdio.h>
#include <string.h>

#include "ble_provisioning_protocol.h"
#include "esp_log.h"
#include "host/ble_gatt.h"
#include "network_provisioning.h"
#include "os/os_mbuf.h"
#include "wifi_remote.h"

static const char *TAG = "BLE";
static uint16_t s_tx_value_handle;

static const ble_uuid128_t s_service_uuid =
    BLE_UUID128_INIT(BLE_PROVISIONING_SERVICE_UUID_BYTES);
static const ble_uuid128_t s_rx_uuid =
    BLE_UUID128_INIT(BLE_PROVISIONING_RX_UUID_BYTES);
static const ble_uuid128_t s_tx_uuid =
    BLE_UUID128_INIT(BLE_PROVISIONING_TX_UUID_BYTES);

static void enqueue_error(int request_id, const char *error)
{
    char json[160];
    snprintf(json, sizeof(json),
             "{\"id\":%d,\"event\":\"error\",\"error\":\"%s\"}",
             request_id, error);
    ble_notify_enqueue_json(json);
}

static void enqueue_current_status(int request_id)
{
    network_status_t status = network_provisioning_get_status();
    status.request_id = request_id;
    if (status.state == NETWORK_STATE_WIFI_CONNECTED ||
        status.state == NETWORK_STATE_WIFI_CONNECTING ||
        status.state == NETWORK_STATE_WIFI_FAILED ||
        status.state == NETWORK_STATE_ERROR ||
        status.state == NETWORK_STATE_WAITING_CREDENTIALS) {
        ble_notify_network_status(&status, NULL);
        return;
    }
    char json[192];
    snprintf(json, sizeof(json),
             "{\"id\":%d,\"event\":\"wifi_state\",\"state\":\"%s\",\"ip\":\"%s\"}",
             request_id, network_provisioning_state_name(status.state), status.ip);
    ble_notify_enqueue_json(json);
}

static void handle_decoded_message(ble_decoded_message_t *message, void *context)
{
    (void)context;
    if (message->error[0] != '\0' || message->command == BLE_COMMAND_INVALID) {
        enqueue_error(message->request_id,
                      message->error[0] != '\0' ? message->error : "INVALID_MESSAGE");
        return;
    }

    if (message->command == BLE_COMMAND_GET_STATUS) {
        enqueue_current_status(message->request_id);
    } else if (message->command == BLE_COMMAND_SCAN_WIFI) {
        esp_err_t err = network_provisioning_submit_scan(message->request_id);
        if (err != ESP_OK) {
            const char *reason = err == ESP_ERR_TIMEOUT
                ? "wifi_scan_queue_full"
                : (err == ESP_ERR_INVALID_STATE
                    ? "wifi_scan_busy" : "wifi_not_ready");
            char json[192];
            snprintf(json, sizeof(json),
                     "{\"id\":%d,\"event\":\"wifi_scan\",\"status\":\"failed\",\"reason\":\"%s\"}",
                     message->request_id, reason);
            ble_notify_enqueue_json(json);
        } else {
            ESP_LOGI(TAG, "board Wi-Fi scan request queued, id=%d",
                     message->request_id);
        }
    } else if (message->command == BLE_COMMAND_GET_DEVICE_INFO) {
        wifi_remote_capabilities_t capabilities = wifi_remote_get_capabilities();
        char json[224];
        snprintf(json, sizeof(json),
                 "{\"id\":%d,\"event\":\"device_info\",\"device\":\"SmartScore-WT99\",\"firmware\":\"smartscore-network-stage1\",\"ble\":%s,\"wifi\":%s}",
                 message->request_id,
                 capabilities.ble_available ? "true" : "false",
                 capabilities.wifi_available ? "true" : "false");
        ble_notify_enqueue_json(json);
    } else if (message->command == BLE_COMMAND_SET_WIFI) {
        esp_err_t err = network_provisioning_submit_credentials(
            message->request_id, message->ssid, message->password);
        if (err == ESP_OK) {
            char json[128];
            snprintf(json, sizeof(json),
                     "{\"id\":%d,\"event\":\"wifi_state\",\"state\":\"connecting\"}",
                     message->request_id);
            ble_notify_enqueue_json(json);
            ESP_LOGI(TAG, "Wi-Fi credentials received, ssid=%s", message->ssid);
        } else {
            enqueue_error(message->request_id,
                          err == ESP_ERR_INVALID_ARG ? "INVALID_CREDENTIALS" : "QUEUE_BUSY");
        }
    } else if (message->command == BLE_COMMAND_RESET_WIFI) {
        esp_err_t err = network_provisioning_submit_reset(message->request_id);
        if (err != ESP_OK) {
            enqueue_error(message->request_id, "QUEUE_BUSY");
        }
    }
}

static int rx_access(uint16_t connection_handle,
                     uint16_t attribute_handle,
                     struct ble_gatt_access_ctxt *context,
                     void *argument)
{
    (void)connection_handle;
    (void)attribute_handle;
    (void)argument;
    if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    esp_err_t codec_result = ESP_OK;
    for (struct os_mbuf *fragment = context->om;
         fragment != NULL;
         fragment = SLIST_NEXT(fragment, om_next)) {
        esp_err_t err = ble_message_codec_feed(
            fragment->om_data,
            fragment->om_len,
            handle_decoded_message,
            NULL);
        if (err != ESP_OK) {
            codec_result = err;
        }
    }
    if (codec_result == ESP_ERR_INVALID_SIZE) {
        enqueue_error(0, "MESSAGE_TOO_LARGE");
    }
    return 0;
}

static int tx_access(uint16_t connection_handle,
                     uint16_t attribute_handle,
                     struct ble_gatt_access_ctxt *context,
                     void *argument)
{
    (void)connection_handle;
    (void)attribute_handle;
    (void)argument;
    if (context->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return ble_notify_append_last(context->om);
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_rx_uuid.u,
                .access_cb = rx_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_tx_uuid.u,
                .access_cb = tx_access,
                .val_handle = &s_tx_value_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {0},
        },
    },
    {0},
};

esp_err_t ble_gatt_server_init(void)
{
    ble_message_codec_reset();
    int result = ble_gatts_count_cfg(s_services);
    if (result == 0) {
        result = ble_gatts_add_svcs(s_services);
    }
    return result == 0 ? ESP_OK : ESP_FAIL;
}

uint16_t ble_gatt_server_tx_handle(void)
{
    return s_tx_value_handle;
}
