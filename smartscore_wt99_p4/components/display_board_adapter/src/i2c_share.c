#include "driver/i2c_master.h"

#include <stdbool.h>

#include "esp_log.h"

static const char *TAG = "SCREEN_I2C";
static i2c_master_bus_handle_t s_borrowed_bus;
static i2c_master_bus_handle_t s_owned_bus;

esp_err_t screen_i2c_new_master_bus(const i2c_master_bus_config_t *bus_config,
                                    i2c_master_bus_handle_t *ret_bus_handle)
{
    if (bus_config == NULL || ret_bus_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_bus_handle_t existing = NULL;
    esp_err_t err = i2c_master_get_bus_handle(bus_config->i2c_port,
                                              &existing);
    if (err == ESP_OK && existing != NULL) {
        s_borrowed_bus = existing;
        *ret_bus_handle = existing;
        ESP_LOGI(TAG, "sharing existing I2C%d bus with audio service",
                 bus_config->i2c_port);
        return ESP_OK;
    }
    if (err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = i2c_new_master_bus(bus_config, ret_bus_handle);
    if (err == ESP_OK) {
        s_owned_bus = *ret_bus_handle;
        ESP_LOGI(TAG, "created display-owned I2C%d bus",
                 bus_config->i2c_port);
    }
    return err;
}

esp_err_t screen_i2c_del_master_bus(i2c_master_bus_handle_t bus_handle)
{
    if (bus_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bus_handle == s_borrowed_bus) {
        s_borrowed_bus = NULL;
        ESP_LOGI(TAG, "released borrowed I2C bus without deleting it");
        return ESP_OK;
    }
    if (bus_handle == s_owned_bus) {
        s_owned_bus = NULL;
    }
    return i2c_del_master_bus(bus_handle);
}
