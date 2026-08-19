#ifndef CREATOR_RECORDER_H
#define CREATOR_RECORDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "midi_parser.h"
#include "creator_quantizer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CREATOR_STAFF_SINGLE = 1,
    CREATOR_STAFF_GRAND = 2,
} creator_staff_mode_t;

typedef enum {
    CREATOR_RECORDER_IDLE = 0,
    CREATOR_RECORDER_RECORDING,
    CREATOR_RECORDER_PAUSED,
} creator_recorder_state_t;

typedef struct {
    int bpm;
    int time_sig_num;
    int time_sig_den;
    creator_staff_mode_t staff_mode;
    uint8_t duration_tolerance_percent;
    uint8_t grand_staff_split_note;
    uint16_t chord_window_ms;
    uint8_t max_notes_per_onset;
} creator_recorder_config_t;

typedef struct {
    uint8_t pitch;
    uint8_t velocity;
    uint8_t channel;
    uint8_t staff;
    uint8_t voice;
    uint64_t start_time_us;
    uint64_t end_time_us;
    uint64_t duration_us;
    uint32_t start_tick;
    uint32_t duration_ticks;
    creator_note_value_t note_value;
    uint8_t dots;
    uint8_t slur_start;
    uint8_t slur_stop;
    uint8_t gliss_start;
    uint8_t gliss_stop;
    bool within_tolerance;
} creator_recorded_note_t;

typedef enum {
    CREATOR_CONNECTION_SLUR = 0,
    CREATOR_CONNECTION_GLISS,
} creator_connection_kind_t;

esp_err_t creator_recorder_init(void);
void creator_recorder_begin(const creator_recorder_config_t *config,
                            uint64_t timestamp_us);
void creator_recorder_stop(void);
void creator_recorder_pause(uint64_t timestamp_us);
void creator_recorder_resume(uint64_t timestamp_us);
bool creator_recorder_resume_from_measure(int measure_number,
                                          uint64_t timestamp_us);
bool creator_recorder_resume_measure_range(int first_measure,
                                           int last_measure,
                                           uint64_t timestamp_us);

bool creator_recorder_note_on(uint8_t channel, uint8_t pitch, uint8_t velocity,
                              uint64_t timestamp_us);
bool creator_recorder_note_off(uint8_t channel, uint8_t pitch,
                               uint64_t timestamp_us);
bool creator_recorder_control_change(uint8_t channel, uint8_t controller,
                                     uint8_t value, uint64_t timestamp_us);
bool creator_recorder_pitch_bend(uint8_t channel, int16_t value,
                                 uint64_t timestamp_us);
/* Explicit semantic input for non-tie connections. Ordinary Note On/Off does
 * not infer slurs or glissandi. Indices address captured logical notes. */
bool creator_recorder_add_connection(creator_connection_kind_t kind,
                                     int start_note_index,
                                     int end_note_index,
                                     uint8_t number);

creator_recorder_state_t creator_recorder_state(void);
bool creator_recorder_waiting_for_first_note(void);
bool creator_recorder_poll(uint64_t timestamp_us);
int creator_recorder_note_count(void);
int creator_recorder_measure_count(void);
uint32_t creator_recorder_measure_ticks(void);
bool creator_recorder_get_note(int index, creator_recorded_note_t *out);
bool creator_recorder_snapshot(midi_data_t *out);
bool creator_recorder_snapshot_live(midi_data_t *out,
                                    uint64_t timestamp_us);

#ifdef __cplusplus
}
#endif

#endif
