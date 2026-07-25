#include "web_provisioning.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "network_provisioning.h"
#include "web_provisioning_internal.h"
#include "wifi_remote.h"

static const char *TAG = "WEB";
static httpd_handle_t s_server;

esp_err_t web_provisioning_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }
    esp_err_t err = wifi_remote_start_apsta("SmartScore_Setup", "", 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "APSTA start failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_t *ap = wifi_remote_get_ap_netif();
    if (ap == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap);
    err = esp_netif_set_ip_info(ap, &ip_info);
    if (err == ESP_OK) {
        err = esp_netif_dhcps_start(ap);
    }
    if (err != ESP_OK) {
        wifi_remote_stop();
        return err;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    err = httpd_start(&s_server, &config);
    if (err == ESP_OK) {
        err = web_provisioning_register_handlers(s_server);
    }
    if (err != ESP_OK) {
        if (s_server != NULL) {
            httpd_stop(s_server);
            s_server = NULL;
        }
        wifi_remote_stop();
        return err;
    }
    network_provisioning_report_web_fallback();
    ESP_LOGI(TAG, "SmartScore_Setup ready at http://192.168.4.1");
    return ESP_OK;
}

esp_err_t web_provisioning_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}

bool web_provisioning_is_running(void)
{
    return s_server != NULL;
}
