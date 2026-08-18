#include "network_provisioning.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "network_provisioning_internal.h"
#include "sdkconfig.h"
#include "wifi_remote.h"

static const char *TAG = "NET";

static QueueHandle_t s_command_queue;
static TaskHandle_t s_task;
static bool s_initialized;
static bool s_started;
static portMUX_TYPE s_operation_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_connect_pending;
static bool s_scan_pending;

#define NETWORK_SCAN_RAW_MAX_APS 32
#define NETWORK_SCAN_NOTIFICATION_DELAY_MS 160

static bool reserve_operation(bool scan)
{
    bool reserved = false;
    portENTER_CRITICAL(&s_operation_lock);
    if (!s_connect_pending && !s_scan_pending) {
        if (scan) {
            s_scan_pending = true;
        } else {
            s_connect_pending = true;
        }
        reserved = true;
    }
    portEXIT_CRITICAL(&s_operation_lock);
    return reserved;
}

static void release_operation(bool scan)
{
    portENTER_CRITICAL(&s_operation_lock);
    if (scan) {
        s_scan_pending = false;
    } else {
        s_connect_pending = false;
    }
    portEXIT_CRITICAL(&s_operation_lock);
}

static bool wifi_reason_is_auth_failure(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return true;
    default:
        return false;
    }
}

static const char *failure_reason_from_wifi(uint8_t reason, esp_err_t wait_error)
{
    if (wait_error == ESP_ERR_TIMEOUT) {
        return "TIMEOUT";
    }
    if (wifi_reason_is_auth_failure(reason)) {
        return "AUTH_FAIL";
    }
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL";
    default:
        return "CONNECT_FAILED";
    }
}

static bool connect_saved_wifi(network_command_t *command,
                               char *ip,
                               size_t ip_size,
                               uint8_t *disconnect_reason,
                               const char **failure_reason,
                               unsigned *attempts_used)
{
    const TickType_t started_at = xTaskGetTickCount();
    const TickType_t budget_ticks = pdMS_TO_TICKS(
        (uint32_t)CONFIG_SMARTSCORE_SAVED_WIFI_TOTAL_TIMEOUT_SECONDS * 1000U);

    *attempts_used = 1;
    esp_err_t err = wifi_remote_start_sta(command->ssid, command->password);
    if (err != ESP_OK) {
        *failure_reason = "START_FAILED";
        return false;
    }

    for (unsigned attempt = 1;
         attempt <= CONFIG_SMARTSCORE_SAVED_WIFI_MAX_ATTEMPTS;
         ++attempt) {
        *attempts_used = attempt;
        TickType_t elapsed_ticks = xTaskGetTickCount() - started_at;
        if (elapsed_ticks >= budget_ticks) {
            *failure_reason = "TIMEOUT";
            break;
        }

        TickType_t remaining_ticks = budget_ticks - elapsed_ticks;
        uint32_t remaining_ms = (uint32_t)(remaining_ticks * portTICK_PERIOD_MS);
        if (remaining_ms == 0) {
            remaining_ms = 1;
        }

        if (attempt > 1) {
            network_state_transition(NETWORK_STATE_WIFI_CONNECTING,
                                     command->request_id,
                                     attempt - 1,
                                     command->ssid,
                                     NULL,
                                     NULL,
                                     false);
        }
        ESP_LOGI("WIFI", "saved Wi-Fi attempt %u/%u, remaining budget=%lums",
                 attempt,
                 (unsigned)CONFIG_SMARTSCORE_SAVED_WIFI_MAX_ATTEMPTS,
                 (unsigned long)remaining_ms);

        err = wifi_remote_wait_for_connection(
            remaining_ms, ip, ip_size, disconnect_reason);
        if (err == ESP_OK) {
            return true;
        }

        *failure_reason = failure_reason_from_wifi(*disconnect_reason, err);
        if (wifi_reason_is_auth_failure(*disconnect_reason) ||
            attempt == CONFIG_SMARTSCORE_SAVED_WIFI_MAX_ATTEMPTS) {
            break;
        }

        elapsed_ticks = xTaskGetTickCount() - started_at;
        if (elapsed_ticks >= budget_ticks) {
            *failure_reason = "TIMEOUT";
            break;
        }

        err = wifi_remote_retry_sta();
        if (err != ESP_OK) {
            *failure_reason = "RETRY_FAILED";
            break;
        }
    }
    return false;
}

static void clear_secret(network_command_t *command)
{
    if (command != NULL) {
        volatile char *bytes = command->password;
        for (size_t index = 0; index < sizeof(command->password); ++index) {
            bytes[index] = 0;
        }
    }
}

static void process_connect(network_command_t *command)
{
    network_state_transition(NETWORK_STATE_WIFI_CONNECTING,
                             command->request_id,
                             0,
                             command->ssid,
                             NULL,
                             NULL,
                             false);
    ESP_LOGI(TAG, "[WIFI] credentials accepted, ssid=%s", command->ssid);

    char ip[16] = {0};
    uint8_t disconnect_reason = 0;
    const char *failure_reason = "CONNECT_FAILED";
    bool connected = false;
    unsigned attempts_used = 0;
    esp_err_t err = ESP_OK;

    if (command->from_saved_credentials) {
        connected = connect_saved_wifi(command,
                                       ip,
                                       sizeof(ip),
                                       &disconnect_reason,
                                       &failure_reason,
                                       &attempts_used);
    } else {
        err = wifi_remote_start_sta(command->ssid, command->password);
        if (err != ESP_OK) {
            network_state_transition(NETWORK_STATE_WIFI_FAILED,
                                     command->request_id,
                                     0,
                                     command->ssid,
                                     NULL,
                                     "START_FAILED",
                                     false);
            clear_secret(command);
            return;
        }

        for (unsigned attempt = 1;
             attempt <= CONFIG_SMARTSCORE_WIFI_MAX_RETRIES;
             ++attempt) {
            if (attempt > 1) {
                unsigned backoff_seconds = 1U << (attempt - 2);
                if (backoff_seconds > 8) {
                    backoff_seconds = 8;
                }
                ESP_LOGI("WIFI", "retry %u/%u after %us",
                         attempt,
                         (unsigned)CONFIG_SMARTSCORE_WIFI_MAX_RETRIES,
                         backoff_seconds);
                network_state_transition(NETWORK_STATE_WIFI_CONNECTING,
                                         command->request_id,
                                         attempt - 1,
                                         command->ssid,
                                         NULL,
                                         NULL,
                                         false);
                vTaskDelay(pdMS_TO_TICKS(backoff_seconds * 1000U));
                err = wifi_remote_retry_sta();
                if (err != ESP_OK) {
                    failure_reason = "RETRY_FAILED";
                    break;
                }
            }

            err = wifi_remote_wait_for_connection(
                CONFIG_SMARTSCORE_WIFI_ATTEMPT_TIMEOUT_SECONDS * 1000U,
                ip,
                sizeof(ip),
                &disconnect_reason);
            if (err == ESP_OK) {
                connected = true;
                break;
            }
            failure_reason = failure_reason_from_wifi(disconnect_reason, err);
        }
    }

    if (!connected) {
        esp_err_t stop_error = wifi_remote_stop();
        if (stop_error != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi stop after connection failure failed: %s",
                     esp_err_to_name(stop_error));
        }
        if (command->from_saved_credentials) {
            network_state_transition(NETWORK_STATE_WAITING_CREDENTIALS,
                                     command->request_id,
                                     0,
                                     NULL,
                                     NULL,
                                     NULL,
                                     false);
            ESP_LOGW(TAG,
                     "saved Wi-Fi unavailable after %u attempt(s), reason=%s; waiting for BLE credentials",
                     attempts_used,
                     failure_reason);
        } else {
            network_state_transition(NETWORK_STATE_WIFI_FAILED,
                                     command->request_id,
                                     CONFIG_SMARTSCORE_WIFI_MAX_RETRIES,
                                     command->ssid,
                                     NULL,
                                     failure_reason,
                                     false);
            ESP_LOGW(TAG, "Wi-Fi connection failed, ssid=%s reason=%s",
                     command->ssid, failure_reason);
        }
        clear_secret(command);
        return;
    }

    err = network_credentials_save(command->ssid, command->password);
    clear_secret(command);
    if (err != ESP_OK) {
        network_state_transition(NETWORK_STATE_ERROR,
                                 command->request_id,
                                 0,
                                 command->ssid,
                                 ip,
                                 "NVS_SAVE_FAILED",
                                 false);
        ESP_LOGE(TAG, "connected but credential save failed: %s", esp_err_to_name(err));
        return;
    }

    network_state_transition(NETWORK_STATE_WIFI_CONNECTED,
                             command->request_id,
                             0,
                             command->ssid,
                             ip,
                             NULL,
                             true);
    ESP_LOGI(TAG, "Wi-Fi connected, ssid=%s ip=%s", command->ssid, ip);
}

static void process_reset(const network_command_t *command)
{
    esp_err_t err = network_credentials_clear();
    esp_err_t stop_error = wifi_remote_stop();
    if (err != ESP_OK || stop_error != ESP_OK) {
        network_state_transition(NETWORK_STATE_ERROR,
                                 command->request_id,
                                 0,
                                 NULL,
                                 NULL,
                                 "RESET_FAILED",
                                 false);
        return;
    }
    network_state_transition(NETWORK_STATE_WAITING_CREDENTIALS,
                             command->request_id,
                             0,
                             NULL,
                             NULL,
                             NULL,
                             false);
    ESP_LOGI(TAG, "saved Wi-Fi configuration cleared");
}

static size_t select_scan_results(const wifi_ap_record_t *records,
                                  size_t record_count,
                                  wifi_ap_record_t *selected,
                                  size_t selected_capacity)
{
    size_t selected_count = 0;
    for (size_t index = 0; index < record_count; ++index) {
        const char *ssid = (const char *)records[index].ssid;
        if (strnlen(ssid, sizeof(records[index].ssid)) == 0) {
            continue;
        }

        size_t existing = selected_count;
        for (size_t candidate = 0; candidate < selected_count; ++candidate) {
            if (strncmp((const char *)selected[candidate].ssid,
                        ssid,
                        sizeof(records[index].ssid)) == 0) {
                existing = candidate;
                break;
            }
        }
        if (existing < selected_count) {
            if (records[index].rssi > selected[existing].rssi) {
                selected[existing] = records[index];
            }
            continue;
        }
        if (selected_count < selected_capacity) {
            selected[selected_count++] = records[index];
            continue;
        }

        size_t weakest = 0;
        for (size_t candidate = 1; candidate < selected_count; ++candidate) {
            if (selected[candidate].rssi < selected[weakest].rssi) {
                weakest = candidate;
            }
        }
        if (records[index].rssi > selected[weakest].rssi) {
            selected[weakest] = records[index];
        }
    }

    for (size_t left = 0; left < selected_count; ++left) {
        for (size_t right = left + 1; right < selected_count; ++right) {
            if (selected[right].rssi > selected[left].rssi) {
                wifi_ap_record_t temporary = selected[left];
                selected[left] = selected[right];
                selected[right] = temporary;
            }
        }
    }
    return selected_count;
}

static void publish_scan_failure(int request_id, const char *reason)
{
    network_scan_event_t event = {
        .type = NETWORK_SCAN_EVENT_FAILED,
        .request_id = request_id,
    };
    strlcpy(event.failure_reason,
            reason != NULL ? reason : "wifi_scan_failed",
            sizeof(event.failure_reason));
    network_scan_events_publish(&event);
}

static void process_scan(const network_command_t *command)
{
    wifi_ap_record_t *records = NULL;
    wifi_ap_record_t *selected = NULL;
    const char *failure_reason = NULL;
    network_scan_event_t event = {
        .type = NETWORK_SCAN_EVENT_STARTED,
        .request_id = command->request_id,
    };
    ESP_LOGI("WIFI", "board Wi-Fi scan requested, stack free minimum=%u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    esp_err_t err = network_scan_events_publish(&event);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan start notification unavailable: %s",
                 esp_err_to_name(err));
        goto cleanup;
    }

    ESP_LOGI("WIFI", "board Wi-Fi scan started");
    records = calloc(NETWORK_SCAN_RAW_MAX_APS, sizeof(*records));
    selected = calloc(NETWORK_PROVISIONING_SCAN_MAX_RESULTS,
                      sizeof(*selected));
    if (records == NULL || selected == NULL) {
        ESP_LOGE("WIFI", "Wi-Fi scan buffer allocation failed");
        failure_reason = "wifi_scan_no_memory";
        goto failed;
    }

    uint16_t record_count = NETWORK_SCAN_RAW_MAX_APS;
    err = wifi_remote_scan(records, &record_count);
    if (err != ESP_OK) {
        ESP_LOGW("WIFI", "board Wi-Fi scan failed: %s", esp_err_to_name(err));
        failure_reason = "wifi_scan_failed";
        goto failed;
    }

    size_t selected_count = select_scan_results(
        records,
        record_count,
        selected,
        NETWORK_PROVISIONING_SCAN_MAX_RESULTS);

    for (size_t index = 0; index < selected_count; ++index) {
        memset(&event, 0, sizeof(event));
        event.type = NETWORK_SCAN_EVENT_NETWORK;
        event.request_id = command->request_id;
        strlcpy(event.ssid,
                (const char *)selected[index].ssid,
                sizeof(event.ssid));
        event.rssi = selected[index].rssi;
        event.channel = selected[index].primary;
        event.open = selected[index].authmode == WIFI_AUTH_OPEN;
        err = network_scan_events_publish(&event);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi scan notification stopped: %s",
                     esp_err_to_name(err));
            goto cleanup;
        }
        vTaskDelay(pdMS_TO_TICKS(NETWORK_SCAN_NOTIFICATION_DELAY_MS));
    }

    memset(&event, 0, sizeof(event));
    event.type = NETWORK_SCAN_EVENT_COMPLETED;
    event.request_id = command->request_id;
    event.count = selected_count;
    err = network_scan_events_publish(&event);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan completion notification unavailable: %s",
                 esp_err_to_name(err));
    }
    ESP_LOGI("WIFI", "board Wi-Fi scan complete, results=%u",
             (unsigned)selected_count);
    goto cleanup;

failed:
    publish_scan_failure(command->request_id, failure_reason);

cleanup:
    free(selected);
    free(records);
    ESP_LOGI("WIFI", "board Wi-Fi scan released buffers, stack free minimum=%u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void provisioning_task(void *argument)
{
    (void)argument;
    network_command_t command;
    while (true) {
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (command.type == NETWORK_COMMAND_CONNECT) {
            process_connect(&command);
            release_operation(false);
        } else if (command.type == NETWORK_COMMAND_RESET) {
            process_reset(&command);
        } else if (command.type == NETWORK_COMMAND_SCAN) {
            process_scan(&command);
            release_operation(true);
        }
        clear_secret(&command);
        memset(&command, 0, sizeof(command));
    }
}

esp_err_t network_provisioning_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    network_state_machine_init();
    network_scan_events_init();
    portENTER_CRITICAL(&s_operation_lock);
    s_connect_pending = false;
    s_scan_pending = false;
    portEXIT_CRITICAL(&s_operation_lock);
    s_command_queue = xQueueCreate(4, sizeof(network_command_t));
    if (s_command_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t network_provisioning_start(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_started) {
        return ESP_OK;
    }
    BaseType_t created = xTaskCreate(provisioning_task,
                                     "network_provisioning",
                                     6144,
                                     NULL,
                                     5,
                                     &s_task);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;

#if CONFIG_SMARTSCORE_AUTO_CONNECT_SAVED_WIFI
    char ssid[33] = {0};
    char password[65] = {0};
    esp_err_t err = network_credentials_load(
        ssid, sizeof(ssid), password, sizeof(password));
    if (err == ESP_OK) {
        network_command_t command = {
            .type = NETWORK_COMMAND_CONNECT,
            .request_id = 0,
            .from_saved_credentials = true,
        };
        strlcpy(command.ssid, ssid, sizeof(command.ssid));
        strlcpy(command.password, password, sizeof(command.password));
        if (!reserve_operation(false)) {
            clear_secret(&command);
            return ESP_ERR_INVALID_STATE;
        }
        if (xQueueSend(s_command_queue, &command, 0) != pdTRUE) {
            release_operation(false);
            clear_secret(&command);
            return ESP_ERR_TIMEOUT;
        }
        clear_secret(&command);
        ESP_LOGI(TAG, "saved Wi-Fi found, auto-connect queued, ssid=%s", ssid);
    } else {
        network_state_transition(NETWORK_STATE_WAITING_CREDENTIALS,
                                 0, 0, NULL, NULL, NULL, false);
        ESP_LOGI(TAG, "waiting for Wi-Fi credentials");
    }
    volatile char *password_bytes = password;
    for (size_t index = 0; index < sizeof(password); ++index) {
        password_bytes[index] = 0;
    }
#else
    network_state_transition(NETWORK_STATE_WAITING_CREDENTIALS,
                             0, 0, NULL, NULL, NULL, false);
    ESP_LOGI(TAG,
             "saved Wi-Fi auto-connect disabled; waiting for BLE credentials");
#endif
    return ESP_OK;
}

esp_err_t network_provisioning_submit_credentials(int request_id,
                                                  const char *ssid,
                                                  const char *password)
{
    if (!s_started || ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t ssid_length = strlen(ssid);
    size_t password_length = strlen(password);
    if (ssid_length == 0 || ssid_length > 32 || password_length > 64) {
        return ESP_ERR_INVALID_ARG;
    }

    network_command_t command = {
        .type = NETWORK_COMMAND_CONNECT,
        .request_id = request_id,
    };
    memcpy(command.ssid, ssid, ssid_length);
    command.ssid[ssid_length] = '\0';
    memcpy(command.password, password, password_length);
    command.password[password_length] = '\0';

    if (!reserve_operation(false)) {
        clear_secret(&command);
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t queued = xQueueSend(s_command_queue, &command, 0);
    if (queued != pdTRUE) {
        release_operation(false);
    }
    clear_secret(&command);
    return queued == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t network_provisioning_get_saved_credentials(char *ssid,
                                                      size_t ssid_size,
                                                      char *password,
                                                      size_t password_size)
{
    return network_credentials_load(ssid, ssid_size, password, password_size);
}

esp_err_t network_provisioning_submit_reset(int request_id)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    network_command_t command = {
        .type = NETWORK_COMMAND_RESET,
        .request_id = request_id,
    };
    return xQueueSend(s_command_queue, &command, 0) == pdTRUE
        ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t network_provisioning_submit_scan(int request_id)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    network_status_t status = network_state_snapshot();
    if (status.state == NETWORK_STATE_WIFI_CONNECTING ||
        status.state == NETWORK_STATE_WIFI_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!reserve_operation(true)) {
        return ESP_ERR_INVALID_STATE;
    }
    network_command_t command = {
        .type = NETWORK_COMMAND_SCAN,
        .request_id = request_id,
    };
    if (xQueueSend(s_command_queue, &command, 0) != pdTRUE) {
        release_operation(true);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

network_status_t network_provisioning_get_status(void)
{
    return network_state_snapshot();
}

esp_err_t network_provisioning_register_observer(network_status_observer_t callback,
                                                 void *context)
{
    return network_state_add_observer(callback, context);
}

void network_provisioning_unregister_observer(network_status_observer_t callback,
                                              void *context)
{
    network_state_remove_observer(callback, context);
}

esp_err_t network_provisioning_register_scan_observer(
    network_scan_observer_t callback,
    void *context)
{
    return network_scan_events_add_observer(callback, context);
}

void network_provisioning_unregister_scan_observer(
    network_scan_observer_t callback,
    void *context)
{
    network_scan_events_remove_observer(callback, context);
}

const char *network_provisioning_state_name(network_state_t state)
{
    static const char *names[] = {
        "uninitialized", "hosted_starting", "hosted_ready", "ble_advertising",
        "ble_connected", "waiting_credentials", "wifi_connecting",
        "wifi_connected", "wifi_failed", "web_fallback", "error"
    };
    if (state < NETWORK_STATE_UNINITIALIZED || state > NETWORK_STATE_ERROR) {
        return "unknown";
    }
    return names[state];
}

static void lifecycle_transition(network_state_t state)
{
    network_status_t current = network_state_snapshot();
    if (current.state < NETWORK_STATE_WIFI_CONNECTING ||
        state == NETWORK_STATE_WEB_FALLBACK) {
        network_state_transition(state,
                                 current.request_id,
                                 current.retry_count,
                                 current.ssid,
                                 current.ip,
                                 current.failure_reason,
                                 current.credentials_saved);
    }
}

void network_provisioning_report_hosted_starting(void)
{
    lifecycle_transition(NETWORK_STATE_HOSTED_STARTING);
}

void network_provisioning_report_hosted_ready(void)
{
    lifecycle_transition(NETWORK_STATE_HOSTED_READY);
}

void network_provisioning_report_ble_advertising(void)
{
    lifecycle_transition(NETWORK_STATE_BLE_ADVERTISING);
}

void network_provisioning_report_ble_connected(void)
{
    lifecycle_transition(NETWORK_STATE_BLE_CONNECTED);
}

void network_provisioning_report_web_fallback(void)
{
    lifecycle_transition(NETWORK_STATE_WEB_FALLBACK);
}
