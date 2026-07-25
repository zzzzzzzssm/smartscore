#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    S3_VOICE_EVENT_WAKE = 0,
    S3_VOICE_EVENT_TIMEOUT,
    S3_VOICE_EVENT_COMMAND,
} s3_voice_event_type_t;

typedef struct {
    s3_voice_event_type_t type;
    uint8_t command_id;
} s3_voice_event_t;

typedef void (*s3_voice_event_handler_t)(const s3_voice_event_t *event,
                                         void *context);

/** Start the dedicated UART2 link to the voice-recognition S3. */
esp_err_t s3_voice_node_init(s3_voice_event_handler_t handler, void *context);

/** Report whether a received command was applicable to the current page. */
esp_err_t s3_voice_node_send_ack(uint8_t command_id, bool handled);

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
