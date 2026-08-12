#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_ASSISTANT_EVENT_CONNECTED = 0,
    VOICE_ASSISTANT_EVENT_SESSION_STARTED,
    VOICE_ASSISTANT_EVENT_INPUT_COMPLETED,
    VOICE_ASSISTANT_EVENT_SESSION_ENDED,
    VOICE_ASSISTANT_EVENT_ERROR,
} voice_assistant_event_type_t;

typedef struct {
    voice_assistant_event_type_t type;
    esp_err_t error;
    const char *reason;
} voice_assistant_event_t;

typedef void (*voice_assistant_event_handler_t)(
    const voice_assistant_event_t *event,
    void *context);

/**
 * Optional snapshot provider reserved for current score/track context.
 * It must only copy a short text snapshot into output and must not mutate the
 * scoring, practice-advice, UI, or model pipelines.
 */
typedef esp_err_t (*voice_assistant_context_provider_t)(
    char *output,
    size_t output_capacity,
    void *context);

esp_err_t voice_assistant_init(voice_assistant_event_handler_t handler,
                               void *context);
void voice_assistant_set_context_provider(
    voice_assistant_context_provider_t provider,
    void *context);

/** Keep the authenticated WebSocket transport warm while Wi-Fi is usable. */
void voice_assistant_set_network_ready(bool ready);

/** Begin buffering after WAKE while S3 still gives local commands priority. */
esp_err_t voice_assistant_arm(void);

/** MultiNet did not match a local command; commit the buffered utterance. */
esp_err_t voice_assistant_begin(void);

/** A local command won; discard candidate audio without touching old logic. */
void voice_assistant_cancel_candidate(void);

/** Fast, bounded copy called by the voice UART RX task. */
esp_err_t voice_assistant_push_audio(const int16_t *pcm,
                                     size_t sample_count,
                                     uint16_t sequence);

#ifdef __cplusplus
}
#endif
