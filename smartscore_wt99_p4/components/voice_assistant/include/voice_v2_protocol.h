#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *data;
    size_t length;
} voice_v2_json_span_t;

typedef enum {
    VOICE_V2_SERVER_UNKNOWN = 0,
    VOICE_V2_SERVER_SESSION_CREATED,
    VOICE_V2_SERVER_SESSION_UPDATED,
    VOICE_V2_SERVER_INPUT_COMMITTED,
    VOICE_V2_SERVER_INPUT_CLEARED,
    VOICE_V2_SERVER_ASR_STARTED,
    VOICE_V2_SERVER_ASR_DELTA,
    VOICE_V2_SERVER_ASR_COMPLETED,
    VOICE_V2_SERVER_ASR_FAILED,
    VOICE_V2_SERVER_TEXT_DELTA,
    VOICE_V2_SERVER_TEXT_DONE,
    VOICE_V2_SERVER_AUDIO_STARTED,
    VOICE_V2_SERVER_AUDIO_DELTA,
    VOICE_V2_SERVER_AUDIO_DONE,
    VOICE_V2_SERVER_RESPONSE_DONE,
    VOICE_V2_SERVER_RESPONSE_CANCELED,
    VOICE_V2_SERVER_SESSION_CLOSED,
    VOICE_V2_SERVER_IGNORED,
    VOICE_V2_SERVER_ERROR,
} voice_v2_server_kind_t;

typedef struct {
    voice_v2_server_kind_t kind;
    char type[96];
    voice_v2_json_span_t event_id;
    voice_v2_json_span_t session_id;
    voice_v2_json_span_t delta;
    voice_v2_json_span_t transcript;
    voice_v2_json_span_t text;
    voice_v2_json_span_t response_status;
    voice_v2_json_span_t error_code;
    voice_v2_json_span_t error_message;
} voice_v2_server_event_t;

esp_err_t voice_v2_protocol_parse_server_event(
    const char *json,
    size_t json_length,
    voice_v2_server_event_t *out_event);

esp_err_t voice_v2_protocol_build_session_update(
    char *output,
    size_t output_capacity,
    const char *voice,
    const char *instructions,
    bool include_static_configuration,
    size_t *out_length);

esp_err_t voice_v2_protocol_build_audio_append(
    char *output,
    size_t output_capacity,
    const uint8_t *pcm,
    size_t pcm_bytes,
    size_t *out_length);

esp_err_t voice_v2_protocol_build_simple_event(
    char *output,
    size_t output_capacity,
    const char *type,
    size_t *out_length);

/** Decode one independently aligned Base64 span (input length must be /4). */
esp_err_t voice_v2_protocol_decode_audio_span(
    const voice_v2_json_span_t *encoded,
    uint8_t *output,
    size_t output_capacity,
    size_t *out_length);

#ifdef __cplusplus
}
#endif
