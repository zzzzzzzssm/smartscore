#pragma once

#include "esp_err.h"
#include "gui_guider.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bind the main-screen "Other Modes" button to the mode hub.
 *
 * Must be called on the LVGL thread after setup_ui().
 */
esp_err_t screen_modes_init(lv_ui *ui);

#ifdef __cplusplus
}
#endif
