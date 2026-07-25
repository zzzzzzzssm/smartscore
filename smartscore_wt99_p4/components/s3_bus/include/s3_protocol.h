#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    S3_MUSIC_MESSAGE_HELLO = 0,
    S3_MUSIC_MESSAGE_STATUS,
    S3_MUSIC_MESSAGE_PONG,
    S3_MUSIC_MESSAGE_HEARTBEAT,
    S3_MUSIC_MESSAGE_PITCH,
    S3_MUSIC_MESSAGE_NOTE_ON,
    S3_MUSIC_MESSAGE_NOTE_OFF,
    S3_MUSIC_MESSAGE_POLY,
} s3_music_message_type_t;

typedef struct {
    s3_music_message_type_t type;
    uint32_t seq;
    uint32_t sid;
    uint32_t ts_ms;
    uint8_t midi;
    uint8_t velocity;
    bool ready;
    bool stream_enabled;
} s3_music_message_t;

typedef enum {
    S3_PROTOCOL_OK = 0,
    S3_PROTOCOL_INVALID_JSON,
    S3_PROTOCOL_INVALID_FIELD,
    S3_PROTOCOL_UNKNOWN_TYPE,
} s3_protocol_result_t;

/**
 * Parse one NUL-terminated NDJSON payload without its trailing newline.
 *
 * length must point to the terminating NUL. The parser accepts the exact
 * schema emitted by the audio S3; only hello carries source="s3_audio".
 */
s3_protocol_result_t s3_protocol_parse_music_line(
    const char *line,
    size_t length,
    s3_music_message_t *out_message);
