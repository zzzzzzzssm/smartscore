#include "ble_provisioning.h"

#include <stdio.h>
#include <string.h>

#include "ble_provisioning_internal.h"
#include "ble_provisioning_protocol.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "network_provisioning.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "wifi_remote.h"

static const char *TAG = "BLE";
static char s_device_name[32];
static uint8_t s_address_type;
static bool s_initialized;
static bool s_ready;
static bool s_controller_initialized;
static bool s_controller_enabled;
static EventGroupHandle_t s_sync_events;

#define BLE_SYNCED_BIT BIT0

static int gap_event_handler(struct ble_gap_event *event, void *argument);

static esp_err_t stop_remote_controller(void)
{
    esp_err_t first_error = ESP_OK;
    if (s_controller_enabled) {
        esp_err_t err = esp_hosted_bt_controller_disable();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "C5 Bluetooth controller disable failed: %s",
                     esp_err_to_name(err));
            first_error = err;
        }
        s_controller_enabled = false;
    }
    if (s_controller_initialized) {
        esp_err_t err = esp_hosted_bt_controller_deinit(false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "C5 Bluetooth controller deinit failed: %s",
                     esp_err_to_name(err));
            if (first_error == ESP_OK) {
                first_error = err;
            }
        }
        s_controller_initialized = false;
    }
    return first_error;
}

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_device_name;
    fields.name_len = strlen(s_device_name);
    fields.name_is_complete = 1;
    int result = ble_gap_adv_set_fields(&fields);
    if (result != 0) {
        ESP_LOGE(TAG, "advertising fields failed, rc=%d", result);
        return;
    }

    const ble_uuid128_t service_uuid =
        BLE_UUID128_INIT(BLE_PROVISIONING_SERVICE_UUID_BYTES);
    struct ble_hs_adv_fields response = {0};
    response.uuids128 = (ble_uuid128_t *)&service_uuid;
    response.num_uuids128 = 1;
    response.uuids128_is_complete = 1;
    result = ble_gap_adv_rsp_set_fields(&response);
    if (result != 0) {
        ESP_LOGE(TAG, "scan response fields failed, rc=%d", result);
        return;
    }

    struct ble_gap_adv_params parameters = {0};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    result = ble_gap_adv_start(s_address_type,
                               NULL,
                               BLE_HS_FOREVER,
                               &parameters,
                               gap_event_handler,
                               NULL);
    if (result != 0) {
        ESP_LOGE(TAG, "advertising start failed, rc=%d", result);
        return;
    }
    network_provisioning_report_ble_advertising();
    ESP_LOGI(TAG, "advertising started");
}

static void on_sync(void)
{
    int result = ble_hs_id_infer_auto(0, &s_address_type);
    if (result != 0) {
        ESP_LOGE(TAG, "address inference failed, rc=%d", result);
        return;
    }
    s_ready = true;
    wifi_remote_set_ble_capability(true);
    xEventGroupSetBits(s_sync_events, BLE_SYNCED_BIT);
    ESP_LOGI("HOSTED", "BLE capability available");
    start_advertising();
}

static void on_reset(int reason)
{
    s_ready = false;
    wifi_remote_set_ble_capability(false);
    ESP_LOGE(TAG, "NimBLE host reset, reason=%d", reason);
}

static void host_task(void *argument)
{
    (void)argument;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static int gap_event_handler(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ble_notify_set_link(event->connect.conn_handle, false);
            network_provisioning_report_ble_connected();
            ESP_LOGI(TAG, "client connected");
        } else {
            start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ble_notify_set_link(BLE_HS_CONN_HANDLE_NONE, false);
        ESP_LOGI(TAG, "client disconnected, reason=%d", event->disconnect.reason);
        start_advertising();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE: {
        bool enabled = event->subscribe.cur_notify != 0 &&
                       event->subscribe.attr_handle == ble_gatt_server_tx_handle();
        ble_notify_set_link(event->subscribe.conn_handle, enabled);
        if (enabled) {
            ESP_LOGI(TAG, "notifications enabled");
            network_status_t status = network_provisioning_get_status();
            if (status.state == NETWORK_STATE_WIFI_CONNECTED ||
                status.state == NETWORK_STATE_WIFI_CONNECTING ||
                status.state == NETWORK_STATE_WIFI_FAILED ||
                status.state == NETWORK_STATE_ERROR ||
                status.state == NETWORK_STATE_WAITING_CREDENTIALS) {
                ble_notify_network_status(&status, NULL);
            }
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

static esp_err_t build_device_name(void)
{
    uint8_t mac[6];
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(s_device_name, sizeof(s_device_name),
             "SmartScore-WT99-%02X%02X", mac[4], mac[5]);
    return ESP_OK;
}

esp_err_t ble_provisioning_init(void)
{
    if (s_initialized) {
        return s_ready ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = build_device_name();
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "device name: %s", s_device_name);

    s_sync_events = xEventGroupCreate();
    if (s_sync_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * The P4 runs only the NimBLE Host.  Before sending HCI commands over
     * Hosted VHCI, initialise and enable the physical BLE controller on the
     * C5 through ESP-Hosted RPC.  Transport capability alone does not start
     * the controller.
     */
    err = esp_hosted_bt_controller_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C5 Bluetooth controller init failed: %s",
                 esp_err_to_name(err));
        vEventGroupDelete(s_sync_events);
        s_sync_events = NULL;
        return err;
    }
    s_controller_initialized = true;
    ESP_LOGI(TAG, "C5 Bluetooth controller initialized");

    err = esp_hosted_bt_controller_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C5 Bluetooth controller enable failed: %s",
                 esp_err_to_name(err));
        stop_remote_controller();
        vEventGroupDelete(s_sync_events);
        s_sync_events = NULL;
        return err;
    }
    s_controller_enabled = true;
    ESP_LOGI(TAG, "C5 Bluetooth controller enabled");

    int result = nimble_port_init();
    if (result != 0) {
        ESP_LOGE(TAG, "nimble_port_init failed, rc=%d", result);
        stop_remote_controller();
        vEventGroupDelete(s_sync_events);
        s_sync_events = NULL;
        return ESP_FAIL;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    result = ble_svc_gap_device_name_set(s_device_name);
    if (result != 0) {
        nimble_port_deinit();
        return ESP_FAIL;
    }
    err = ble_gatt_server_init();
    if (err == ESP_OK) {
        err = ble_notify_init();
    }
    if (err == ESP_OK) {
        err = network_provisioning_register_observer(
            ble_notify_network_status, NULL);
    }
    if (err == ESP_OK) {
        err = network_provisioning_register_scan_observer(
            ble_notify_scan_event, NULL);
        if (err != ESP_OK) {
            network_provisioning_unregister_observer(
                ble_notify_network_status, NULL);
        }
    }
    if (err != ESP_OK) {
        ble_notify_deinit();
        nimble_port_deinit();
        stop_remote_controller();
        vEventGroupDelete(s_sync_events);
        s_sync_events = NULL;
        return err;
    }

    nimble_port_freertos_init(host_task);
    s_initialized = true;
    EventBits_t bits = xEventGroupWaitBits(
        s_sync_events, BLE_SYNCED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));
    if ((bits & BLE_SYNCED_BIT) == 0) {
        wifi_remote_set_ble_capability(false);
        ESP_LOGE(TAG, "NimBLE synchronization timed out; C5 controller did not respond");
        ble_provisioning_deinit();
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "NimBLE host initialized");
    return ESP_OK;
}

esp_err_t ble_provisioning_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    network_provisioning_unregister_observer(ble_notify_network_status, NULL);
    network_provisioning_unregister_scan_observer(ble_notify_scan_event, NULL);
    ble_notify_deinit();
    int result = nimble_port_stop();
    if (result == 0) {
        result = nimble_port_deinit();
    }
    esp_err_t controller_result = stop_remote_controller();
    if (s_sync_events != NULL) {
        vEventGroupDelete(s_sync_events);
        s_sync_events = NULL;
    }
    s_initialized = false;
    s_ready = false;
    wifi_remote_set_ble_capability(false);
    if (result != 0) {
        return ESP_FAIL;
    }
    return controller_result;
}

bool ble_provisioning_is_ready(void)
{
    return s_ready;
}

const char *ble_provisioning_device_name(void)
{
    return s_device_name;
}
