#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t network_diagnostics_start(void);
esp_err_t network_diagnostics_stop(void);
bool network_diagnostics_is_running(void);

#ifdef __cplusplus
}
#endif
