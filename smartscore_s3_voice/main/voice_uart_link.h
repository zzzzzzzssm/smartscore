#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t voice_uart_link_init(void);
esp_err_t voice_uart_link_send_wake(void);
esp_err_t voice_uart_link_send_timeout(void);
esp_err_t voice_uart_link_send_command(uint8_t command_id);
esp_err_t voice_uart_link_send_ai_begin(void);
esp_err_t voice_uart_link_send_ai_speech_end(void);
esp_err_t voice_uart_link_send_ai_cancel(void);
esp_err_t voice_uart_link_write_binary(const void *data, size_t length);
bool voice_uart_link_take_ai_input_done_request(void);
bool voice_uart_link_take_ai_stop_request(void);

#ifdef __cplusplus
}
#endif
