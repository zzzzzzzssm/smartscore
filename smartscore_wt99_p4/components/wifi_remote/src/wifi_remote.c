#include "wifi_remote.h"

#include <string.h>

#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_wifi_default.h"
#include "esp_wifi_remote.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "HOSTED";

static bool s_platform_initialized;
static bool s_hosted_started;
static bool s_wifi_initialized;
static bool s_wifi_started;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;

static EventGroupHandle_t s_hosted_events;
static esp_event_handler_instance_t s_hosted_handler;

#define HOSTED_READY_BIT BIT0
#define HOSTED_FAILURE_BIT BIT1

static void destroy_default_wifi_netifs(void)
{
    if (s_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_ap_netif != NULL) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
}

static void hosted_event_handler(void *arg,
                                 esp_event_base_t event_base,
                                 int32_t event_id,
                                 void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    if (event_id == ESP_HOSTED_EVENT_CP_INIT ||
        event_id == ESP_HOSTED_EVENT_TRANSPORT_UP) {
        wifi_remote_status_set_hosted(true);
        xEventGroupSetBits(s_hosted_events, HOSTED_READY_BIT);
    } else if (event_id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE ||
               event_id == ESP_HOSTED_EVENT_TRANSPORT_DOWN) {
        wifi_remote_status_set_hosted(false);
        xEventGroupSetBits(s_hosted_events, HOSTED_FAILURE_BIT);
    }
}

static esp_err_t init_once_result(esp_err_t err)
{
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}

esp_err_t wifi_remote_platform_init(void)
{
    if (s_platform_initialized) {
        return ESP_OK;
    }

    esp_err_t err = init_once_result(esp_netif_init());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = init_once_result(esp_event_loop_create_default());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event loop creation failed: %s", esp_err_to_name(err));
        return err;
    }

    s_hosted_events = xEventGroupCreate();
    if (s_hosted_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_remote_status_reset();
    s_platform_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_remote_hosted_start(void)
{
    if (!s_platform_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_hosted_started) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "initialization started");
    esp_err_t err = esp_event_handler_instance_register(
        ESP_HOSTED_EVENT,
        ESP_EVENT_ANY_ID,
        hosted_event_handler,
        NULL,
        &s_hosted_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Hosted event registration failed: %s", esp_err_to_name(err));
        return err;
    }

    int hosted_result = esp_hosted_init();
    if (hosted_result != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_init failed: %s", esp_err_to_name(hosted_result));
        esp_event_handler_instance_unregister(
            ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, s_hosted_handler);
        s_hosted_handler = NULL;
        return hosted_result;
    }

    /*
     * esp_hosted_init() creates the transport and RPC infrastructure, but it
     * does not reset the co-processor or start the SDIO handshake.  Connect
     * explicitly after the event handler is registered so TRANSPORT_UP and
     * CP_INIT cannot be missed.
     */
    xEventGroupClearBits(s_hosted_events, HOSTED_READY_BIT | HOSTED_FAILURE_BIT);
    hosted_result = esp_hosted_connect_to_slave();
    if (hosted_result != ESP_OK) {
        ESP_LOGE(TAG, "ESP-Hosted slave connection failed: %s",
                 esp_err_to_name(hosted_result));
        esp_event_handler_instance_unregister(
            ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, s_hosted_handler);
        s_hosted_handler = NULL;
        return hosted_result;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_hosted_events,
        HOSTED_READY_BIT | HOSTED_FAILURE_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(10000));
    if ((bits & HOSTED_READY_BIT) == 0) {
        ESP_LOGE(TAG, "Hosted transport did not become ready");
        esp_event_handler_instance_unregister(
            ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, s_hosted_handler);
        s_hosted_handler = NULL;
        return (bits & HOSTED_FAILURE_BIT) ? ESP_FAIL : ESP_ERR_TIMEOUT;
    }

    uint32_t chip_id = 0;
    char target_name[20] = {0};
    esp_hosted_coprocessor_fwver_t version = {0};
    char firmware[32] = "unknown";

    if (esp_hosted_get_cp_info(&chip_id, target_name, sizeof(target_name)) != ESP_OK) {
        strlcpy(target_name, "unknown", sizeof(target_name));
    }
    if (esp_hosted_get_coprocessor_fwversion(&version) == ESP_OK) {
        snprintf(firmware, sizeof(firmware), "%lu.%lu.%lu",
                 (unsigned long)version.major1,
                 (unsigned long)version.minor1,
                 (unsigned long)version.patch1);
    }
    wifi_remote_status_set_coprocessor(chip_id, target_name, firmware);

    s_hosted_started = true;
    ESP_LOGI(TAG, "initialization complete");
    ESP_LOGI(TAG, "co-processor=%s chip_id=%lu firmware=%s",
             target_name, (unsigned long)chip_id, firmware);
    return ESP_OK;
}

esp_err_t wifi_remote_init(void)
{
    if (!s_hosted_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_wifi_initialized) {
        return ESP_OK;
    }

    /*
     * esp_wifi_remote injects the remote Wi-Fi implementation behind the
     * standard IDF default-netif helpers for this configuration.  The
     * esp_wifi_remote_create_default_* symbols are only linked by the
     * component's alternate CONFIG_ESP_HOST_WIFI_ENABLED build path.  The
     * default netifs must exist before esp_wifi_init(), matching the Hosted
     * host examples, so their STA_START handlers can initialize MAC and DHCP.
     */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) {
        destroy_default_wifi_netifs();
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init through remote failed: %s", esp_err_to_name(err));
        destroy_default_wifi_netifs();
        return err;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        esp_wifi_deinit();
        destroy_default_wifi_netifs();
        return err;
    }

    err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi band mode: 2.4 GHz + 5 GHz");
    } else {
        ESP_LOGW(TAG, "dual-band mode unavailable, using firmware default: %s",
                 esp_err_to_name(err));
    }

    err = wifi_remote_events_register();
    if (err != ESP_OK) {
        esp_wifi_deinit();
        destroy_default_wifi_netifs();
        return err;
    }

    s_wifi_initialized = true;
    wifi_remote_status_set_initialized(true);
    wifi_remote_status_set_wifi_capability(true);
    ESP_LOGI(TAG, "Wi-Fi capability available");
    return ESP_OK;
}

static esp_err_t stop_if_started(void)
{
    if (!s_wifi_started) {
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_ERR_WIFI_NOT_STARTED) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        s_wifi_started = false;
        wifi_remote_status_set_started(false, WIFI_REMOTE_MODE_STOPPED);
    }
    return err;
}

static esp_err_t validate_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t ssid_len = strlen(ssid);
    size_t password_len = strlen(password);
    if (ssid_len == 0 || ssid_len > 32 || password_len > 64) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void copy_wifi_field(uint8_t *destination,
                            size_t destination_size,
                            const char *source)
{
    size_t length = strlen(source);
    if (length > destination_size) {
        length = destination_size;
    }
    memcpy(destination, source, length);
}

esp_err_t wifi_remote_start_sta(const char *ssid, const char *password)
{
    if (!s_wifi_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = validate_credentials(ssid, password);
    if (err != ESP_OK) {
        return err;
    }
    wifi_config_t config = {0};
    copy_wifi_field(config.sta.ssid, sizeof(config.sta.ssid), ssid);
    copy_wifi_field(config.sta.password, sizeof(config.sta.password), password);
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    wifi_remote_events_clear_connection_bits();
    wifi_remote_status_set_disconnected(0);

    wifi_remote_status_t current = wifi_remote_get_status();
    if (s_wifi_started && current.mode == WIFI_REMOTE_MODE_APSTA) {
        /* Keep the setup AP alive so its page can poll the connection result. */
        err = esp_wifi_set_config(WIFI_IF_STA, &config);
        if (err == ESP_OK) {
            err = esp_wifi_connect();
        }
        if (err != ESP_OK) {
            ESP_LOGE("WIFI", "STA connect in APSTA failed: %s",
                     esp_err_to_name(err));
            return err;
        }
        ESP_LOGI("WIFI", "connecting in APSTA, ssid=%s", ssid);
        return ESP_OK;
    }

    err = stop_if_started();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE("WIFI", "STA start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_wifi_started = true;
    wifi_remote_status_set_started(true, WIFI_REMOTE_MODE_STA);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE("WIFI", "STA connect failed: %s", esp_err_to_name(err));
        stop_if_started();
        return err;
    }
    ESP_LOGI("WIFI", "connecting, ssid=%s", ssid);
    return ESP_OK;
}

esp_err_t wifi_remote_retry_sta(void)
{
    if (!s_wifi_started) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_remote_events_clear_connection_bits();
    return esp_wifi_connect();
}

static esp_err_t configure_ap(const char *ssid,
                              const char *password,
                              uint8_t channel,
                              wifi_config_t *config)
{
    esp_err_t err = validate_credentials(ssid, password);
    if (err != ESP_OK) {
        return err;
    }
    size_t password_len = strlen(password);
    if (password_len > 0 && password_len < 8) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));
    copy_wifi_field(config->ap.ssid, sizeof(config->ap.ssid), ssid);
    config->ap.ssid_len = strlen(ssid);
    copy_wifi_field(config->ap.password, sizeof(config->ap.password), password);
    config->ap.channel = (channel >= 1 && channel <= 13) ? channel : 1;
    config->ap.max_connection = 4;
    config->ap.authmode = password_len == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config->ap.pmf_cfg.capable = true;
    config->ap.pmf_cfg.required = false;
    return ESP_OK;
}

static esp_err_t start_ap_mode(const char *ssid,
                               const char *password,
                               uint8_t channel,
                               wifi_mode_t idf_mode,
                               wifi_remote_mode_t remote_mode)
{
    if (!s_wifi_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_config_t config;
    esp_err_t err = configure_ap(ssid, password, channel, &config);
    if (err != ESP_OK) {
        return err;
    }
    err = stop_if_started();
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(idf_mode);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &config);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        return err;
    }

    s_wifi_started = true;
    wifi_remote_status_set_started(true, remote_mode);
    ESP_LOGI("WIFI", "%s started, ssid=%s",
             remote_mode == WIFI_REMOTE_MODE_APSTA ? "APSTA" : "AP", ssid);
    return ESP_OK;
}

esp_err_t wifi_remote_start_ap(const char *ssid, const char *password, uint8_t channel)
{
    return start_ap_mode(ssid, password, channel,
                         WIFI_MODE_AP, WIFI_REMOTE_MODE_AP);
}

esp_err_t wifi_remote_start_apsta(const char *ssid, const char *password, uint8_t channel)
{
    return start_ap_mode(ssid, password, channel,
                         WIFI_MODE_APSTA, WIFI_REMOTE_MODE_APSTA);
}

esp_err_t wifi_remote_disable_ap(void)
{
    if (!s_wifi_started) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_remote_status_t status = wifi_remote_get_status();
    if (status.mode != WIFI_REMOTE_MODE_APSTA) {
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        wifi_remote_status_set_started(true, WIFI_REMOTE_MODE_STA);
        ESP_LOGI("WIFI", "fallback AP disabled; STA remains active");
    }
    return err;
}

esp_err_t wifi_remote_stop(void)
{
    esp_err_t err = stop_if_started();
    wifi_remote_status_set_disconnected(0);
    return err;
}

esp_err_t wifi_remote_scan(wifi_ap_record_t *records, uint16_t *count)
{
    if (records == NULL || count == NULL || *count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_wifi_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint16_t record_capacity = *count;
    wifi_remote_status_t status = wifi_remote_get_status();
    esp_err_t err = ESP_OK;
    if (!s_wifi_started) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) {
            err = esp_wifi_start();
        }
        if (err != ESP_OK) {
            ESP_LOGE("WIFI", "scan-only STA start failed: %s",
                     esp_err_to_name(err));
            return err;
        }
        s_wifi_started = true;
        wifi_remote_status_set_started(true, WIFI_REMOTE_MODE_STA);
        ESP_LOGI("WIFI", "scan-only STA started");
    } else if (status.mode == WIFI_REMOTE_MODE_AP) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            return err;
        }
        wifi_remote_status_set_started(true, WIFI_REMOTE_MODE_APSTA);
    }

    wifi_scan_config_t scan = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 0,
        .scan_time.active.max = 120,
    };
    wifi_remote_events_prepare_scan();
    err = esp_wifi_scan_start(&scan, false);
    if (err != ESP_OK) {
        ESP_LOGE("WIFI", "asynchronous scan start failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    uint16_t event_result_count = 0;
    err = wifi_remote_events_wait_for_scan(30000, &event_result_count);
    if (err != ESP_OK) {
        ESP_LOGE("WIFI", "asynchronous scan did not complete: %s",
                 esp_err_to_name(err));
        return err;
    }

    uint16_t available = 0;
    err = esp_wifi_scan_get_ap_num(&available);
    if (err != ESP_OK) {
        return err;
    }
    if (available == 0) {
        *count = 0;
        ESP_LOGI("WIFI", "scan complete, event results=%u records=0",
                 event_result_count);
        return ESP_OK;
    }

    *count = available < record_capacity ? available : record_capacity;
    err = esp_wifi_scan_get_ap_records(count, records);
    if (err == ESP_OK) {
        ESP_LOGI("WIFI", "scan complete, event results=%u records=%u",
                 event_result_count, *count);
    }
    return err;
}

esp_netif_t *wifi_remote_get_ap_netif(void)
{
    return s_ap_netif;
}

esp_err_t wifi_remote_deinit(void)
{
    esp_err_t first_error = wifi_remote_stop();
    if (s_wifi_initialized) {
        wifi_remote_events_unregister();
        esp_err_t err = esp_wifi_deinit();
        if (first_error == ESP_OK && err != ESP_OK) {
            first_error = err;
        }
        destroy_default_wifi_netifs();
        s_wifi_initialized = false;
        wifi_remote_status_set_initialized(false);
        wifi_remote_status_set_wifi_capability(false);
    }

    if (s_hosted_started) {
        int hosted_error = esp_hosted_deinit();
        if (first_error == ESP_OK && hosted_error != ESP_OK) {
            first_error = hosted_error;
        }
        s_hosted_started = false;
        wifi_remote_status_set_hosted(false);
    }
    if (s_hosted_handler != NULL) {
        esp_event_handler_instance_unregister(
            ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, s_hosted_handler);
        s_hosted_handler = NULL;
    }
    return first_error;
}
