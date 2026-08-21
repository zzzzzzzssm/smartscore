#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "music_detector.h"
#include "piano_note_tracker.h"

/* Initializes UART1, the non-blocking input queues and MusicLinkTxTask. */
esp_err_t music_uart_link_init(void);

/* Announces whether the audio detector completed startup. */
void music_uart_link_set_ready(bool ready);

/* Both functions are non-blocking. Queue overflow only increments tx_drop. */
void music_uart_link_submit_result(const music_result_t *result);
void music_uart_link_submit_pitch(const music_result_t *result);

/* Non-blocking latest-state note set (protocol v2 "notes" events). */
void music_uart_link_submit_note_set(const piano_note_set_t *note_set);
