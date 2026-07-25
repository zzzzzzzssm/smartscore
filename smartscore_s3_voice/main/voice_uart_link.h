#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t voice_uart_link_init(void);
esp_err_t voice_uart_link_send_wake(void);
esp_err_t voice_uart_link_send_timeout(void);
esp_err_t voice_uart_link_send_command(uint8_t command_id);

#ifdef __cplusplus
}
#endif
