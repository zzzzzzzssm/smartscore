#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Confirm the fixed board-level 3.3 V rail used by the ESP32-C5. */
esp_err_t board_wt99_network_power_enable(void);

/** Validate the reset GPIO; ESP-Hosted owns the actual reset pulse. */
esp_err_t board_wt99_c5_reset(void);

/** Validate the compiled Hosted transport against the WT99 1V1 schematic. */
esp_err_t board_wt99_c5_transport_prepare(void);

/** True when the compiled Hosted settings exactly match WT99 revision 1V1. */
bool board_wt99_network_supported(void);

/** Human-readable configuration error suitable for diagnostics. */
const char *board_wt99_network_block_reason(void);

#ifdef __cplusplus
}
#endif
