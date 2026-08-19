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
    S3_VOICE_EVENT_AI_STATE,
    S3_VOICE_EVENT_AI_AUDIO_START,
    S3_VOICE_EVENT_AI_AUDIO_DONE,
    S3_VOICE_EVENT_AI_ERROR,
    S3_VOICE_EVENT_AI_TEXT,
} s3_voice_event_type_t;

typedef struct {
    s3_voice_event_type_t type;
    uint8_t command_id;
    uint8_t ai_state;
    uint32_t sample_rate_hz;
    const char *text;
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

/** Forward the existing provisioned Wi-Fi credentials to the voice S3. */
esp_err_t s3_voice_node_send_wifi_credentials(const char *ssid,
                                               const char *password);

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
    s3_camera_practice_state_t state,
    const char *practice_session_id);

/** Read the number of photos associated with one completed practice. */
esp_err_t s3_camera_node_list_photos(const char *practice_session_id,
                                     uint32_t *out_photo_count);

typedef esp_err_t (*s3_camera_photo_chunk_handler_t)(
    const uint8_t *data,
    size_t length,
    size_t total_length,
    bool first_chunk,
    void *context);

/** Stream one JPEG from the camera S3 without buffering the whole file. */
esp_err_t s3_camera_node_stream_photo(
    const char *practice_session_id,
    uint32_t photo_index,
    s3_camera_photo_chunk_handler_t handler,
    void *context);

#ifdef __cplusplus
}
#endif
