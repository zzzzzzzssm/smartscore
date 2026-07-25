#include "network_diagnostics.h"

#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "network_provisioning.h"

static const char *TAG = "HTTP";
static httpd_handle_t s_server;

static esp_err_t send_status_json(httpd_req_t *request, bool ping)
{
    network_status_t status = network_provisioning_get_status();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }

    bool connected = status.state == NETWORK_STATE_WIFI_CONNECTED;
    cJSON_AddBoolToObject(root, "ok", connected);
    cJSON_AddStringToObject(root, "device", "SmartScore-WT99");
    if (ping) {
        cJSON_AddStringToObject(root, "network", connected ? "connected" : "disconnected");
    } else {
        cJSON_AddStringToObject(root, "state",
                                network_provisioning_state_name(status.state));
        cJSON_AddNumberToObject(root, "retry_count", status.retry_count);
        if (status.failure_reason[0] != '\0') {
            cJSON_AddStringToObject(root, "reason", status.failure_reason);
        }
    }
    cJSON_AddStringToObject(root, "ip", status.ip);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(request, json);
    cJSON_free(json);
    return err;
}

static esp_err_t ping_handler(httpd_req_t *request)
{
    return send_status_json(request, true);
}

static esp_err_t status_handler(httpd_req_t *request)
{
    return send_status_json(request, false);
}

esp_err_t network_diagnostics_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }
    network_status_t status = network_provisioning_get_status();
    if (status.state != NETWORK_STATE_WIFI_CONNECTED ||
        status.ip[0] == '\0' ||
        strcmp(status.ip, "0.0.0.0") == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/api/ping", .method = HTTP_GET, .handler = ping_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
    };
    for (size_t index = 0; index < sizeof(routes) / sizeof(routes[0]); ++index) {
        err = httpd_register_uri_handler(s_server, &routes[index]);
        if (err != ESP_OK) {
            httpd_stop(s_server);
            s_server = NULL;
            return err;
        }
    }
    ESP_LOGI(TAG, "/api/ping ready at http://%s/api/ping", status.ip);
    return ESP_OK;
}

esp_err_t network_diagnostics_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}

bool network_diagnostics_is_running(void)
{
    return s_server != NULL;
}
