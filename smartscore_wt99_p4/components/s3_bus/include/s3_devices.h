#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    S3_VOICE_EVENT_WAKE = 0,
    S3_VOICE_EVENT_TIMEOUT,
    S3_VOICE_EVENT_COMMAND,
    S3_VOICE_EVENT_AI_BEGIN,
    S3_VOICE_EVENT_SPEECH_END,
    S3_VOICE_EVENT_AI_CANCEL,
} s3_voice_event_type_t;

typedef struct {
    s3_voice_event_type_t type;
    uint8_t command_id;
} s3_voice_event_t;

typedef void (*s3_voice_event_handler_t)(const s3_voice_event_t *event,
                                         void *context);

typedef esp_err_t (*s3_voice_audio_handler_t)(const int16_t *pcm,
                                              size_t sample_count,
                                              uint16_t sequence,
                                              void *context);

/** Start the dedicated UART2 link to the voice-recognition S3. */
esp_err_t s3_voice_node_init(s3_voice_event_handler_t handler, void *context);

/** Report whether a received command was applicable to the current page. */
esp_err_t s3_voice_node_send_ack(uint8_t command_id, bool handled);

/** Register the non-blocking PCM consumer for AI candidate/conversation audio. */
void s3_voice_node_set_audio_handler(s3_voice_audio_handler_t handler,
                                     void *context);

/** Ask the voice S3 to leave AI streaming and restore local wake mode. */
esp_err_t s3_voice_node_send_ai_stop(const char *reason);

/** Stop S3 microphone upload after one utterance while the answer plays. */
esp_err_t s3_voice_node_send_ai_input_done(void);

typedef struct {
    uint8_t command_id;
} s3_camera_event_t;

typedef void (*s3_camera_event_handler_t)(const s3_camera_event_t *event,
                                          void *context);

/** Start the dedicated UART3 link to the camera/gesture-recognition S3. */
esp_err_t s3_camera_node_init(s3_camera_event_handler_t handler,
                              void *context);

/** Report whether a received gesture command was applicable to the page. */
esp_err_t s3_camera_node_send_ack(uint8_t command_id, bool handled);

typedef enum {
    S3_CAMERA_PRACTICE_PLAYING = 0,
    S3_CAMERA_PRACTICE_PAUSED,
    S3_CAMERA_PRACTICE_FINISHED,
} s3_camera_practice_state_t;

/** Synchronize the real P4 practice state to the camera S3. */
esp_err_t s3_camera_node_send_practice_state(
    s3_camera_practice_state_t state);

#ifdef __cplusplus
}
#endif
