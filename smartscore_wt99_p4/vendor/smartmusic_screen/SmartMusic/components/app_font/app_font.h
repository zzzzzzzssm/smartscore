#ifndef APP_FONT_H
#define APP_FONT_H

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_FONT_VFS_PATTERN  "/sdcard/fonts/HanSansSCBold_24_c%d.bin"
#define APP_FONT_CHUNK_COUNT  7
#define APP_FONT_CHINESE_SIZE 24

/* Call after LVGL is initialized and the SD card is mounted. */
esp_err_t app_font_init(void);
bool app_font_is_ready(void);
const lv_font_t *app_font_chinese_22(void);
void app_font_apply_missing_cjk(lv_obj_t *root);

#ifdef __cplusplus
}
#endif

#endif
