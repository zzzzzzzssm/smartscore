#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define S3_PROTOCOL_POLY_MAX_NOTES 4
#define S3_PROTOCOL_POLY_NAME_MAX 16
#define S3_PROTOCOL_NOTE_SET_MAX_NOTES 4
#define S3_PROTOCOL_DIAGNOSTIC_MAX_RAW 4
#define S3_PROTOCOL_DIAGNOSTIC_MAX_NOTES 3
#define S3_PROTOCOL_DIAGNOSTIC_REASON_MAX 32

typedef enum {
    S3_MUSIC_MESSAGE_HELLO = 0,
    S3_MUSIC_MESSAGE_STATUS,
    S3_MUSIC_MESSAGE_PONG,
    S3_MUSIC_MESSAGE_HEARTBEAT,
    S3_MUSIC_MESSAGE_PITCH,
    S3_MUSIC_MESSAGE_NOTE_ON,
    S3_MUSIC_MESSAGE_NOTE_OFF,
    S3_MUSIC_MESSAGE_POLY,
    S3_MUSIC_MESSAGE_DIAGNOSTIC,
    S3_MUSIC_MESSAGE_NOTES,
} s3_music_message_type_t;

typedef enum {
    S3_PROTOCOL_POLY_NONE = 0,
    S3_PROTOCOL_POLY_INTERVAL,
    S3_PROTOCOL_POLY_CHORD,
} s3_protocol_poly_kind_t;

typedef enum {
    S3_PROTOCOL_RESULT_UNKNOWN = 0,
    S3_PROTOCOL_RESULT_SILENCE,
    S3_PROTOCOL_RESULT_SINGLE,
    S3_PROTOCOL_RESULT_INTERVAL,
    S3_PROTOCOL_RESULT_CHORD,
} s3_protocol_result_kind_t;

typedef struct {
    uint8_t source;
    uint8_t midi;
    float frequency_hz;
    float confidence;
} s3_protocol_diagnostic_candidate_t;

typedef struct {
    s3_music_message_type_t type;
    uint32_t seq;
    uint32_t sid;
    uint32_t ts_ms;
    uint32_t duration_ms;
    uint32_t state_id;
    uint32_t version;
    uint8_t midi;
    uint8_t velocity;
    float frequency_hz;
    float confidence;
    bool has_duration;
    bool has_frequency;
    bool has_confidence;
    bool ready;
    bool stream_enabled;
    s3_protocol_poly_kind_t poly_kind;
    uint8_t note_count;
    uint8_t notes[S3_PROTOCOL_POLY_MAX_NOTES];
    char poly_name[S3_PROTOCOL_POLY_NAME_MAX];
    uint8_t note_set_count;
    uint8_t midis[S3_PROTOCOL_NOTE_SET_MAX_NOTES];
    uint8_t velocities[S3_PROTOCOL_NOTE_SET_MAX_NOTES];
    float confidences[S3_PROTOCOL_NOTE_SET_MAX_NOTES];
    float set_confidence;
    bool degraded_mic;
    bool overflow;
    uint8_t diagnostic_raw_count;
    s3_protocol_diagnostic_candidate_t
        diagnostic_raw[S3_PROTOCOL_DIAGNOSTIC_MAX_RAW];
    s3_protocol_result_kind_t diagnostic_candidate_kind;
    uint8_t diagnostic_candidate_count;
    uint8_t diagnostic_candidate_notes[S3_PROTOCOL_DIAGNOSTIC_MAX_NOTES];
    s3_protocol_result_kind_t diagnostic_final_kind;
    uint8_t diagnostic_final_count;
    uint8_t diagnostic_final_notes[S3_PROTOCOL_DIAGNOSTIC_MAX_NOTES];
    int8_t diagnostic_octave_shift;
    float diagnostic_snr_db[3];
    char diagnostic_reject[S3_PROTOCOL_DIAGNOSTIC_REASON_MAX];
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
