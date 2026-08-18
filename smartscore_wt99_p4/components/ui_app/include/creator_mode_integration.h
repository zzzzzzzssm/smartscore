#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Return an active Creator page directly to home.
 *
 * Must be called on the LVGL thread. An active, unsaved recording is canceled
 * with the same semantics as the existing Creator "Exit" action.
 */
bool creator_mode_voice_go_home(lv_obj_t *home_screen);

#ifdef __cplusplus
}
#endif
