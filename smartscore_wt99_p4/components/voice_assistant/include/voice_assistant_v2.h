#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_ASSISTANT_V2_EVENT_CONNECTED = 0,
    VOICE_ASSISTANT_V2_EVENT_SESSION_STARTED,
    VOICE_ASSISTANT_V2_EVENT_INPUT_COMPLETED,
    VOICE_ASSISTANT_V2_EVENT_SESSION_ENDED,
    VOICE_ASSISTANT_V2_EVENT_ERROR,
} voice_assistant_v2_event_type_t;

typedef struct {
    voice_assistant_v2_event_type_t type;
    esp_err_t error;
    const char *reason;
} voice_assistant_v2_event_t;

typedef void (*voice_assistant_v2_event_handler_t)(
    const voice_assistant_v2_event_t *event,
    void *context);

typedef esp_err_t (*voice_assistant_v2_context_provider_t)(
    char *output,
    size_t output_capacity,
    void *context);

esp_err_t voice_assistant_v2_init(
    voice_assistant_v2_event_handler_t handler,
    void *context);

void voice_assistant_v2_set_context_provider(
    voice_assistant_v2_context_provider_t provider,
    void *context);

/** Keep the authenticated transport warm while networking is available. */
void voice_assistant_v2_set_network_ready(bool ready);

/** Start a cloud candidate immediately after WAKE. */
esp_err_t voice_assistant_v2_arm(void);

/** Confirm that the current candidate is an open AI request. */
esp_err_t voice_assistant_v2_begin(void);

/** Ordered S3 marker: all PCM for this utterance precedes this event. */
esp_err_t voice_assistant_v2_speech_end(void);

/** A local command won. The owner task serializes cancel/close. */
void voice_assistant_v2_cancel_candidate(void);

/** Bounded PSRAM copy called by the S3 UART receive task. */
esp_err_t voice_assistant_v2_push_audio(const int16_t *pcm,
                                        size_t sample_count,
                                        uint16_t sequence);

#ifdef __cplusplus
}
#endif
