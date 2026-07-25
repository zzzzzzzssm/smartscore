#include "web_provisioning_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "network_provisioning.h"
#include "provisioning_page.h"
#include "wifi_remote.h"

#define MAX_FORM_BYTES 512
#define MAX_SCAN_RESULTS 20

static const char *TAG = "WEB";

static esp_err_t send_json(httpd_req_t *request, cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    if (json == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(request, json);
    cJSON_free(json);
    return err;
}

static esp_err_t root_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, SMARTSCORE_PROVISIONING_PAGE);
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool url_decode(const char *input, size_t input_length,
                       char *output, size_t output_size)
{
    size_t used = 0;
    for (size_t index = 0; index < input_length; ++index) {
        if (used + 1 >= output_size) return false;
        if (input[index] == '+') {
            output[used++] = ' ';
        } else if (input[index] == '%' && index + 2 < input_length) {
            int high = hex_value(input[index + 1]);
            int low = hex_value(input[index + 2]);
            if (high < 0 || low < 0) return false;
            output[used++] = (char)((high << 4) | low);
            index += 2;
        } else {
            output[used++] = input[index];
        }
    }
    output[used] = '\0';
    return true;
}

static bool form_value(const char *body, const char *key,
                       char *output, size_t output_size)
{
    size_t key_length = strlen(key);
    const char *cursor = body;
    while (*cursor != '\0') {
        const char *end = strchr(cursor, '&');
        if (end == NULL) end = cursor + strlen(cursor);
        const char *equals = memchr(cursor, '=', (size_t)(end - cursor));
        if (equals != NULL && (size_t)(equals - cursor) == key_length &&
            memcmp(cursor, key, key_length) == 0) {
            return url_decode(equals + 1, (size_t)(end - equals - 1),
                              output, output_size);
        }
        cursor = *end == '\0' ? end : end + 1;
    }
    return false;
}

static esp_err_t connect_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len > MAX_FORM_BYTES) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid form size");
    }
    char *body = calloc(1, request->content_len + 1);
    if (body == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    int received = 0;
    while (received < request->content_len) {
        int count = httpd_req_recv(request, body + received,
                                   request->content_len - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (count <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += count;
    }

    char ssid[33] = {0};
    char password[65] = {0};
    bool valid = form_value(body, "ssid", ssid, sizeof(ssid)) &&
                 form_value(body, "password", password, sizeof(password));
    free(body);
    if (!valid || ssid[0] == '\0') {
        memset(password, 0, sizeof(password));
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "SSID is required");
    }
    esp_err_t err = network_provisioning_submit_credentials(0, ssid, password);
    memset(password, 0, sizeof(password));
    if (err != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "request queue busy");
    }
    ESP_LOGI(TAG, "web credentials accepted, ssid=%s", ssid);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(request, "连接请求已接收，请等待状态更新。");
}

static esp_err_t reset_handler(httpd_req_t *request)
{
    esp_err_t err = network_provisioning_submit_reset(0);
    if (err != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "request queue busy");
    }
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(request, "配置清除请求已接收，不会重启设备。");
}

static esp_err_t scan_handler(httpd_req_t *request)
{
    wifi_ap_record_t records[MAX_SCAN_RESULTS];
    uint16_t count = MAX_SCAN_RESULTS;
    esp_err_t err = wifi_remote_scan(records, &count);

    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_CreateArray();
    if (root == NULL || networks == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(networks);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddItemToObject(root, "networks", networks);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    } else {
        for (uint16_t index = 0; index < count; ++index) {
            if (records[index].ssid[0] == '\0') continue;
            cJSON *item = cJSON_CreateObject();
            if (item == NULL) continue;
            cJSON_AddStringToObject(item, "ssid", (const char *)records[index].ssid);
            cJSON_AddNumberToObject(item, "rssi", records[index].rssi);
            cJSON_AddNumberToObject(item, "channel", records[index].primary);
            cJSON_AddBoolToObject(item, "open", records[index].authmode == WIFI_AUTH_OPEN);
            cJSON_AddItemToArray(networks, item);
        }
    }
    err = send_json(request, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t status_handler(httpd_req_t *request)
{
    network_status_t status = network_provisioning_get_status();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    cJSON_AddBoolToObject(root, "ok", status.state == NETWORK_STATE_WIFI_CONNECTED);
    cJSON_AddStringToObject(root, "state", network_provisioning_state_name(status.state));
    cJSON_AddStringToObject(root, "ip", status.ip);
    cJSON_AddNumberToObject(root, "retry_count", status.retry_count);
    if (status.failure_reason[0] != '\0') {
        cJSON_AddStringToObject(root, "reason", status.failure_reason);
    }
    esp_err_t err = send_json(request, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t web_provisioning_register_handlers(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_handler},
        {.uri = "/connect", .method = HTTP_POST, .handler = connect_handler},
        {.uri = "/reset", .method = HTTP_GET, .handler = reset_handler},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = scan_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
    };
    for (size_t index = 0; index < sizeof(handlers) / sizeof(handlers[0]); ++index) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[index]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
