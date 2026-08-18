#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    QWEN_VOICE_IDLE = 0,
    QWEN_VOICE_CANDIDATE,
    QWEN_VOICE_LISTENING,
    QWEN_VOICE_THINKING,
    QWEN_VOICE_SPEAKING,
    QWEN_VOICE_DRAINING,
    QWEN_VOICE_ERROR,
} qwen_voice_state_t;

esp_err_t qwen_realtime_init(void);
esp_err_t qwen_realtime_set_wifi_credentials(const char *ssid,
                                              const char *password);
esp_err_t qwen_realtime_arm(void);
esp_err_t qwen_realtime_begin(void);
esp_err_t qwen_realtime_speech_end(void);
void qwen_realtime_cancel_local(void);
void qwen_realtime_request_stop(void);
void qwen_realtime_set_uart_flow(bool enabled);
void qwen_realtime_notify_p4_drained(void);
esp_err_t qwen_realtime_push_pcm(const int16_t *pcm, size_t sample_count);
qwen_voice_state_t qwen_realtime_state(void);

