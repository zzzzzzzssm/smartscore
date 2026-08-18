#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "gui_guider.h"
#include "s3_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bind the main-screen "Other Modes" button to the mode hub.
 *
 * Must be called on the LVGL thread after setup_ui().
 */
esp_err_t screen_modes_init(lv_ui *ui);

/**
 * Handle Voice S3 "return home" for pages owned by Other Modes.
 *
 * Must be called on the LVGL thread. Returns false when the active page is
 * not owned by Other Modes so the caller can continue with normal navigation.
 */
bool screen_modes_voice_go_home(void);

/* Non-LVGL Audio S3 event ingress; copies data and returns immediately. */
void screen_modes_handle_audio_s3_event(const s3_music_event_t *event);

#ifdef __cplusplus
}
#endif
