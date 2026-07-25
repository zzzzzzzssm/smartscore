#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_provisioning_init(void);
esp_err_t ble_provisioning_deinit(void);
bool ble_provisioning_is_ready(void);
const char *ble_provisioning_device_name(void);

#ifdef __cplusplus
}
#endif
