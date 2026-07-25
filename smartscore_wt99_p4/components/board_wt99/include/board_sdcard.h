#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_SDCARD_MOUNT_POINT "/sdcard"
#define BOARD_SDCARD_WAV_DIRECTORY "/sdcard/wav"

typedef struct {
    bool mounted;
    uint64_t capacity_bytes;
    esp_err_t last_error;
} board_sdcard_status_t;

esp_err_t board_sdcard_mount(void);
void board_sdcard_get_status(board_sdcard_status_t *out_status);

#ifdef __cplusplus
}
#endif
