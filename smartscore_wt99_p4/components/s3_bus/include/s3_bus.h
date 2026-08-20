#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define S3_MUSIC_POLY_MAX_NOTES 4
#define S3_MUSIC_POLY_NAME_MAX 16
#define S3_MUSIC_NOTE_SET_MAX_NOTES 4
#define S3_MUSIC_DIAGNOSTIC_MAX_RAW 4
#define S3_MUSIC_DIAGNOSTIC_MAX_NOTES 3
#define S3_MUSIC_DIAGNOSTIC_REASON_MAX 32

typedef enum {
    S3_MUSIC_EVENT_NOTE_ON = 0,
    S3_MUSIC_EVENT_NOTE_OFF,
    S3_MUSIC_EVENT_POLY,
    S3_MUSIC_EVENT_DIAGNOSTIC,
} s3_music_event_type_t;

typedef enum {
    S3_MUSIC_POLY_NONE = 0,
    S3_MUSIC_POLY_INTERVAL,
    S3_MUSIC_POLY_CHORD,
} s3_music_poly_kind_t;

typedef enum {
    S3_MUSIC_RESULT_UNKNOWN = 0,
    S3_MUSIC_RESULT_SILENCE,
    S3_MUSIC_RESULT_SINGLE,
    S3_MUSIC_RESULT_INTERVAL,
    S3_MUSIC_RESULT_CHORD,
} s3_music_result_kind_t;

typedef struct {
    uint8_t source;
    uint8_t midi;
    float frequency_hz;
    float confidence;
} s3_music_diagnostic_candidate_t;

typedef enum {
    S3_MUSIC_STREAM_PROFILE_STRICT = 0,
    S3_MUSIC_STREAM_PROFILE_DEMO,
    S3_MUSIC_STREAM_PROFILE_PERFORMANCE,
} s3_music_stream_profile_t;

typedef struct {
    s3_music_event_type_t type;
    uint32_t seq;
    uint32_t sid;
    uint32_t sender_ts_ms;
    uint32_t duration_ms;
    uint8_t midi;
    uint8_t velocity;
    float frequency_hz;
    float confidence;
    bool has_duration;
    bool has_frequency;
    bool has_confidence;
    s3_music_poly_kind_t poly_kind;
    uint8_t note_count;
    uint8_t notes[S3_MUSIC_POLY_MAX_NOTES];
    char poly_name[S3_MUSIC_POLY_NAME_MAX];
    uint8_t diagnostic_raw_count;
    s3_music_diagnostic_candidate_t
        diagnostic_raw[S3_MUSIC_DIAGNOSTIC_MAX_RAW];
    s3_music_result_kind_t diagnostic_candidate_kind;
    uint8_t diagnostic_candidate_count;
    uint8_t diagnostic_candidate_notes[S3_MUSIC_DIAGNOSTIC_MAX_NOTES];
    s3_music_result_kind_t diagnostic_final_kind;
    uint8_t diagnostic_final_count;
    uint8_t diagnostic_final_notes[S3_MUSIC_DIAGNOSTIC_MAX_NOTES];
    int8_t diagnostic_octave_shift;
    float diagnostic_snr_db[3];
    char diagnostic_reject[S3_MUSIC_DIAGNOSTIC_REASON_MAX];
} s3_music_event_t;

typedef void (*s3_music_event_handler_t)(const s3_music_event_t *event,
                                         void *context);

typedef struct {
    bool initialized;
    bool online;
    bool ready;
    bool stream_enabled;
    bool stream_requested;
    bool command_link_confirmed;
    uint32_t last_rx_ms;
    uint32_t last_pong_ms;
    uint32_t last_sender_ts_ms;
    uint32_t last_sid;
    uint32_t last_seq;
    uint32_t last_state_id;
    uint32_t ack_state_id;
    uint32_t polls_sent;
    uint32_t tx_commands;
    uint32_t tx_failures;
    uint32_t rx_bytes;
    uint32_t valid_frames;
    uint32_t invalid_frames;
    uint32_t oversized_lines;
    uint32_t duplicate_frames;
    uint32_t dropped_events;
    uint32_t atomic_set_retries;
    uint8_t active_note_count;
    bool degraded_mic;
    bool overflow;
} s3_bus_status_t;

/**
 * Initialize the dedicated audio-S3 UART receiver and event dispatcher.
 *
 * The handler runs in the s3_music_dispatch task, never in the UART RX task.
 * Calling this function again after a successful initialization is harmless.
 */
esp_err_t s3_bus_init(s3_music_event_handler_t handler, void *context);

/** Send a ping command; a matching pong confirms the P4-to-S3 path. */
esp_err_t s3_bus_ping(void);

/** Start a new S3 recognition stream session. */
esp_err_t s3_bus_start_stream(uint32_t sid);

/** Start a stream with an explicit detector profile. */
esp_err_t s3_bus_start_stream_with_profile(uint32_t sid,
                                           s3_music_stream_profile_t profile);

/** Stop S3 recognition events while leaving heartbeat/status online. */
esp_err_t s3_bus_stop_stream(void);

/** Return a coherent status snapshot. */
void s3_bus_get_status(s3_bus_status_t *out_status);

#ifdef __cplusplus
}
#endif
