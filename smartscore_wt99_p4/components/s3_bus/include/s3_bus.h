#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    S3_MUSIC_EVENT_NOTE_ON = 0,
    S3_MUSIC_EVENT_NOTE_OFF,
} s3_music_event_type_t;

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
    uint32_t polls_sent;
    uint32_t tx_commands;
    uint32_t tx_failures;
    uint32_t rx_bytes;
    uint32_t valid_frames;
    uint32_t invalid_frames;
    uint32_t oversized_lines;
    uint32_t duplicate_frames;
    uint32_t dropped_events;
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

/** Stop S3 recognition events while leaving heartbeat/status online. */
esp_err_t s3_bus_stop_stream(void);

/** Return a coherent status snapshot. */
void s3_bus_get_status(s3_bus_status_t *out_status);

#ifdef __cplusplus
}
#endif
