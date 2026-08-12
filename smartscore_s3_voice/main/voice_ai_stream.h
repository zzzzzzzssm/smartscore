#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t voice_ai_stream_init(void);
void voice_ai_stream_start_session(void);
void voice_ai_stream_stop_session(void);
bool voice_ai_stream_is_active(void);

/* Non-blocking duplicate of AFE output; local recognition remains the owner. */
esp_err_t voice_ai_stream_push_pcm(const int16_t *pcm, size_t sample_count);

/* Queue an ordered marker behind every PCM frame accepted for this turn. */
esp_err_t voice_ai_stream_finish_utterance(void);

/* Called by the UART RX task for cumulative ACK / replay requests. */
void voice_ai_stream_handle_feedback(uint8_t packet_type,
                                     uint16_t sequence);

#ifdef __cplusplus
}
#endif
