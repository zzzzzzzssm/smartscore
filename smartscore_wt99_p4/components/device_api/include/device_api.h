#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the single HTTP API server after Wi-Fi has an IP address. */
esp_err_t device_api_start(void);

/** Stop the HTTP API server. Repeated calls are safe. */
esp_err_t device_api_stop(void);

bool device_api_is_running(void);

#ifdef __cplusplus
}
#endif
