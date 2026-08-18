#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wake_prompt_init(void);

/* Queues the SD-card wake prompt and returns immediately. */
esp_err_t wake_prompt_play(void);

#ifdef __cplusplus
}
#endif
