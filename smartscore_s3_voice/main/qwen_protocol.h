#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    const char *data;
    size_t length;
} qwen_json_span_t;

typedef enum {
    QWEN_SERVER_UNKNOWN = 0,
    QWEN_SERVER_SESSION_CREATED,
    QWEN_SERVER_SESSION_UPDATED,
    QWEN_SERVER_INPUT_COMMITTED,
    QWEN_SERVER_INPUT_CLEARED,
    QWEN_SERVER_ASR_STARTED,
    QWEN_SERVER_ASR_COMPLETED,
    QWEN_SERVER_TEXT_DELTA,
    QWEN_SERVER_TEXT_DONE,
    QWEN_SERVER_AUDIO_DELTA,
    QWEN_SERVER_AUDIO_DONE,
    QWEN_SERVER_RESPONSE_DONE,
    QWEN_SERVER_RESPONSE_CANCELED,
    QWEN_SERVER_SESSION_CLOSED,
    QWEN_SERVER_IGNORED,
    QWEN_SERVER_ERROR,
} qwen_server_kind_t;

typedef struct {
    qwen_server_kind_t kind;
    char type[96];
    qwen_json_span_t delta;
    qwen_json_span_t transcript;
    qwen_json_span_t text;
    qwen_json_span_t error_code;
    qwen_json_span_t error_message;
} qwen_server_event_t;

esp_err_t qwen_protocol_parse_server_event(const char *json,
                                            size_t json_length,
                                            qwen_server_event_t *event);
esp_err_t qwen_protocol_build_session_update(char *output,
                                              size_t capacity,
                                              const char *voice,
                                              const char *instructions,
                                              size_t *output_length);
esp_err_t qwen_protocol_build_audio_append(char *output,
                                            size_t capacity,
                                            const uint8_t *pcm,
                                            size_t pcm_bytes,
                                            size_t *output_length);
esp_err_t qwen_protocol_build_simple_event(char *output,
                                            size_t capacity,
                                            const char *type,
                                            size_t *output_length);
esp_err_t qwen_protocol_decode_audio(const qwen_json_span_t *encoded,
                                      uint8_t *output,
                                      size_t capacity,
                                      size_t *output_length);

