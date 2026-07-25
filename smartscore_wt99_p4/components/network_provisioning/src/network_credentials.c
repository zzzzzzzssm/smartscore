#include "network_provisioning_internal.h"

#include <string.h>

#include "nvs.h"

#define NVS_NAMESPACE "smartscore_net"
#define NVS_KEY_SSID "wifi_ssid"
#define NVS_KEY_PASSWORD "wifi_pass"
#define NVS_KEY_VALID "wifi_valid"

esp_err_t network_credentials_load(char *ssid,
                                   size_t ssid_size,
                                   char *password,
                                   size_t password_size)
{
    if (ssid == NULL || password == NULL || ssid_size == 0 || password_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ssid[0] = '\0';
    password[0] = '\0';

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t valid = 0;
    err = nvs_get_u8(handle, NVS_KEY_VALID, &valid);
    if (err == ESP_OK && valid == 1) {
        size_t length = ssid_size;
        err = nvs_get_str(handle, NVS_KEY_SSID, ssid, &length);
    }
    if (err == ESP_OK && valid == 1) {
        size_t length = password_size;
        err = nvs_get_str(handle, NVS_KEY_PASSWORD, password, &length);
    }
    nvs_close(handle);

    if (err == ESP_OK && (valid != 1 || ssid[0] == '\0')) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    return err;
}

esp_err_t network_credentials_save(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_PASSWORD, password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, NVS_KEY_VALID, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t network_credentials_clear(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
