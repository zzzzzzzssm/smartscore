#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called from the LVGL task/event context. */
void practice_advice_view_open(lv_obj_t *return_screen);

#ifdef __cplusplus
}
#endif
