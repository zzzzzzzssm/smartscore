#include "creator_recorder.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define CREATOR_MAX_ACTIVE_NOTES 32
#define CREATOR_MIDI_CHANNELS 16
#define CREATOR_MIDI_PITCHES 128
#define CREATOR_DEFAULT_CHORD_WINDOW_MS 45
#define CREATOR_DEFAULT_MAX_NOTES_PER_ONSET 10

typedef struct {
    bool used;
    bool key_released;
    uint8_t channel;
    uint8_t pitch;
    uint8_t velocity;
    uint64_t start_time_us;
    uint32_t start_tick;
} active_note_t;

typedef struct {
    SemaphoreHandle_t mutex;
    creator_recorder_config_t config;
    creator_recorder_state_t state;
    uint64_t segment_origin_us;
    uint32_t segment_start_tick;
    uint32_t append_tick;
    bool segment_started;
    bool range_recording;
    uint32_t range_end_tick;
    creator_recorded_note_t notes[MAX_NOTES];
    int note_count;
    active_note_t active[CREATOR_MAX_ACTIVE_NOTES];
    bool sustain_pedal[CREATOR_MIDI_CHANNELS];
    midi_raw_event_t raw_events[MAX_MIDI_RAW_EVENTS];
    int raw_event_count;
    int raw_event_start;
    bool onset_group_valid;
    uint64_t onset_group_time_us;
    uint32_t onset_group_tick;
    uint8_t onset_group_count;
    uint8_t onset_group_pitches[CREATOR_MIDI_PITCHES / 8];
    uint8_t suppressed_note_on[CREATOR_MIDI_CHANNELS][CREATOR_MIDI_PITCHES];
} creator_recorder_context_t;

static creator_recorder_context_t s_recorder;

static uint8_t dots_for_exact_ticks(uint32_t ticks);

static void lock_recorder(void)
{
    if (s_recorder.mutex) xSemaphoreTake(s_recorder.mutex, portMAX_DELAY);
}

static void unlock_recorder(void)
{
    if (s_recorder.mutex) xSemaphoreGive(s_recorder.mutex);
}

static creator_recorder_config_t normalized_config(
    const creator_recorder_config_t *config)
{
    creator_recorder_config_t result = config ? *config :
        (creator_recorder_config_t){
            .bpm = 120,
            .time_sig_num = 4,
            .time_sig_den = 4,
            .staff_mode = CREATOR_STAFF_GRAND,
            .duration_tolerance_percent = 30,
            .grand_staff_split_note = 60,
            .chord_window_ms = CREATOR_DEFAULT_CHORD_WINDOW_MS,
            .max_notes_per_onset = CREATOR_DEFAULT_MAX_NOTES_PER_ONSET,
        };
    if (result.bpm < 20 || result.bpm > 300) result.bpm = 120;
    if (result.time_sig_num < 1 || result.time_sig_num > 12)
        result.time_sig_num = 4;
    if (result.time_sig_den != 4 && result.time_sig_den != 8)
        result.time_sig_den = 4;
    if (result.staff_mode != CREATOR_STAFF_SINGLE &&
        result.staff_mode != CREATOR_STAFF_GRAND)
        result.staff_mode = CREATOR_STAFF_GRAND;
    if (!result.duration_tolerance_percent)
        result.duration_tolerance_percent = 30;
    if (!result.grand_staff_split_note)
        result.grand_staff_split_note = 60;
    if (!result.chord_window_ms)
        result.chord_window_ms = CREATOR_DEFAULT_CHORD_WINDOW_MS;
    if (result.chord_window_ms > 100) result.chord_window_ms = 100;
    if (!result.max_notes_per_onset)
        result.max_notes_per_onset = CREATOR_DEFAULT_MAX_NOTES_PER_ONSET;
    if (result.max_notes_per_onset > 16) result.max_notes_per_onset = 16;
    return result;
}

static uint32_t measure_ticks_locked(void)
{
    return CREATOR_TICKS_PER_QUARTER * 4U *
           (uint32_t)s_recorder.config.time_sig_num /
           (uint32_t)s_recorder.config.time_sig_den;
}

static uint32_t current_tick_locked(uint64_t timestamp_us)
{
    if (!s_recorder.segment_started) return s_recorder.segment_start_tick;
    uint64_t elapsed = timestamp_us > s_recorder.segment_origin_us ?
                       timestamp_us - s_recorder.segment_origin_us : 0;
    return s_recorder.segment_start_tick +
           creator_time_us_to_ticks(elapsed, s_recorder.config.bpm);
}

static void reset_input_filter_locked(void)
{
    s_recorder.onset_group_valid = false;
    s_recorder.onset_group_time_us = 0;
    s_recorder.onset_group_tick = 0;
    s_recorder.onset_group_count = 0;
    memset(s_recorder.onset_group_pitches, 0,
           sizeof(s_recorder.onset_group_pitches));
    memset(s_recorder.suppressed_note_on, 0,
           sizeof(s_recorder.suppressed_note_on));
}

static bool onset_group_has_pitch_locked(uint8_t pitch)
{
    return (s_recorder.onset_group_pitches[pitch >> 3] &
            (uint8_t)(1U << (pitch & 7))) != 0;
}

static void onset_group_add_pitch_locked(uint8_t pitch)
{
    s_recorder.onset_group_pitches[pitch >> 3] |=
        (uint8_t)(1U << (pitch & 7));
    s_recorder.onset_group_count++;
}

static uint64_t minimum_onset_interval_us_locked(void)
{
    int bpm = s_recorder.config.bpm > 0 ? s_recorder.config.bpm : 120;
    return 60000000ULL / ((uint64_t)bpm * 4ULL);
}

static bool accept_note_on_locked(uint8_t pitch, uint64_t timestamp_us,
                                  uint32_t *start_tick)
{
    uint32_t quantized_tick =
        creator_quantize_onset_tick(current_tick_locked(timestamp_us));
    if (!s_recorder.onset_group_valid) {
        s_recorder.onset_group_valid = true;
        s_recorder.onset_group_time_us = timestamp_us;
        s_recorder.onset_group_tick = quantized_tick;
        s_recorder.onset_group_count = 0;
        memset(s_recorder.onset_group_pitches, 0,
               sizeof(s_recorder.onset_group_pitches));
    } else {
        uint64_t elapsed = timestamp_us > s_recorder.onset_group_time_us ?
                           timestamp_us - s_recorder.onset_group_time_us : 0;
        uint64_t chord_window =
            (uint64_t)s_recorder.config.chord_window_ms * 1000ULL;
        if (elapsed > chord_window) {
            if (elapsed < minimum_onset_interval_us_locked()) return false;
            s_recorder.onset_group_time_us = timestamp_us;
            s_recorder.onset_group_tick = quantized_tick;
            s_recorder.onset_group_count = 0;
            memset(s_recorder.onset_group_pitches, 0,
                   sizeof(s_recorder.onset_group_pitches));
        }
    }
    if (onset_group_has_pitch_locked(pitch) ||
        s_recorder.onset_group_count >=
            s_recorder.config.max_notes_per_onset)
        return false;
    onset_group_add_pitch_locked(pitch);
    *start_tick = s_recorder.onset_group_tick;
    return true;
}

static void assign_staff_locked(uint8_t pitch, uint8_t *staff, uint8_t *voice)
{
    if (s_recorder.config.staff_mode == CREATOR_STAFF_GRAND &&
        pitch < s_recorder.config.grand_staff_split_note) {
        *staff = 2;
        *voice = 2;
    } else {
        *staff = 1;
        *voice = 1;
    }
}

static bool finish_active_locked(active_note_t *active, uint64_t timestamp_us)
{
    if (!active || !active->used || s_recorder.note_count >= MAX_NOTES)
        return false;

    uint64_t duration_us = timestamp_us > active->start_time_us ?
                           timestamp_us - active->start_time_us : 1;
    creator_quantized_duration_t quantized = creator_quantize_duration(
        duration_us, s_recorder.config.bpm,
        s_recorder.config.duration_tolerance_percent);
    creator_recorded_note_t *note = &s_recorder.notes[s_recorder.note_count++];
    memset(note, 0, sizeof(*note));
    note->pitch = active->pitch;
    note->velocity = active->velocity;
    note->channel = active->channel;
    note->start_time_us = active->start_time_us;
    note->end_time_us = timestamp_us;
    note->duration_us = duration_us;
    note->start_tick = active->start_tick;
    note->duration_ticks = quantized.ticks;
    bool clipped = s_recorder.range_recording &&
        note->start_tick < s_recorder.range_end_tick &&
        note->duration_ticks > s_recorder.range_end_tick - note->start_tick;
    if (clipped)
        note->duration_ticks = s_recorder.range_end_tick - note->start_tick;
    note->note_value = quantized.value;
    note->dots = clipped ? dots_for_exact_ticks(note->duration_ticks) :
                 quantized.dots;
    note->within_tolerance = quantized.within_tolerance;
    assign_staff_locked(note->pitch, &note->staff, &note->voice);

    uint32_t end_tick = note->start_tick + note->duration_ticks;
    if (end_tick > s_recorder.append_tick) s_recorder.append_tick = end_tick;
    active->used = false;
    return true;
}

static void retain_raw_event_locked(midi_raw_event_type_t type,
                                    uint8_t channel, uint8_t data1,
                                    int16_t value, uint64_t timestamp_us)
{
    int index;
    if (s_recorder.raw_event_count < MAX_MIDI_RAW_EVENTS) {
        index = (s_recorder.raw_event_start + s_recorder.raw_event_count) %
                MAX_MIDI_RAW_EVENTS;
        s_recorder.raw_event_count++;
    } else {
        index = s_recorder.raw_event_start;
        s_recorder.raw_event_start =
            (s_recorder.raw_event_start + 1) % MAX_MIDI_RAW_EVENTS;
    }
    s_recorder.raw_events[index] = (midi_raw_event_t){
        .tick = current_tick_locked(timestamp_us),
        .type = type,
        .channel = channel & 0x0f,
        .data1 = data1,
        .value = value,
    };
}

static void finish_all_active_locked(uint64_t timestamp_us)
{
    for (int i = 0; i < CREATOR_MAX_ACTIVE_NOTES; ++i)
        if (s_recorder.active[i].used)
            finish_active_locked(&s_recorder.active[i], timestamp_us);
}

static void recompute_append_tick_locked(void)
{
    s_recorder.append_tick = 0;
    for (int i = 0; i < s_recorder.note_count; ++i) {
        uint32_t end_tick = s_recorder.notes[i].start_tick +
                            s_recorder.notes[i].duration_ticks;
        if (end_tick > s_recorder.append_tick)
            s_recorder.append_tick = end_tick;
    }
}

static void remove_dangling_connections_locked(void)
{
    for (int i = 0; i < s_recorder.note_count; ++i) {
        creator_recorded_note_t *note = &s_recorder.notes[i];
        if (note->slur_start) {
            bool found = false;
            for (int j = i + 1; j < s_recorder.note_count; ++j)
                if (s_recorder.notes[j].slur_stop == note->slur_start)
                    found = true;
            if (!found) note->slur_start = 0;
        }
        if (note->gliss_start) {
            bool found = false;
            for (int j = i + 1; j < s_recorder.note_count; ++j)
                if (s_recorder.notes[j].gliss_stop == note->gliss_start)
                    found = true;
            if (!found) note->gliss_start = 0;
        }
        if (note->slur_stop) {
            bool found = false;
            for (int j = 0; j < i; ++j)
                if (s_recorder.notes[j].slur_start == note->slur_stop)
                    found = true;
            if (!found) note->slur_stop = 0;
        }
        if (note->gliss_stop) {
            bool found = false;
            for (int j = 0; j < i; ++j)
                if (s_recorder.notes[j].gliss_start == note->gliss_stop)
                    found = true;
            if (!found) note->gliss_stop = 0;
        }
    }
}

static bool range_end_reached_locked(uint64_t timestamp_us)
{
    return s_recorder.range_recording && s_recorder.segment_started &&
           current_tick_locked(timestamp_us) >= s_recorder.range_end_tick;
}

static void enter_paused_locked(uint64_t timestamp_us, bool range_completed)
{
    uint32_t completed_end = s_recorder.range_end_tick;
    finish_all_active_locked(timestamp_us);
    if (range_completed && s_recorder.append_tick < completed_end)
        s_recorder.append_tick = completed_end;
    reset_input_filter_locked();
    s_recorder.segment_started = false;
    s_recorder.range_recording = false;
    s_recorder.range_end_tick = 0;
    s_recorder.state = CREATOR_RECORDER_PAUSED;
}

static int compare_midi_notes(const void *lhs, const void *rhs)
{
    const midi_note_t *a = lhs;
    const midi_note_t *b = rhs;
    if (a->start_tick < b->start_tick) return -1;
    if (a->start_tick > b->start_tick) return 1;
    if (a->staff < b->staff) return -1;
    if (a->staff > b->staff) return 1;
    return (int)a->note - (int)b->note;
}

static uint8_t dots_for_exact_ticks(uint32_t ticks)
{
    static const uint32_t dotted_ticks[] = {
        CREATOR_TICKS_PER_QUARTER * 6,
        CREATOR_TICKS_PER_QUARTER * 3,
        CREATOR_TICKS_PER_QUARTER * 3 / 2,
        CREATOR_TICKS_PER_QUARTER * 3 / 4,
        CREATOR_TICKS_PER_QUARTER * 3 / 8,
    };
    for (size_t i = 0; i < sizeof(dotted_ticks) / sizeof(dotted_ticks[0]); ++i)
        if (ticks == dotted_ticks[i]) return 1;
    return 0;
}

static void append_split_note_locked(midi_data_t *out,
                                     const creator_recorded_note_t *source,
                                     uint32_t measure_ticks)
{
    if (!out || !source || !source->duration_ticks || !measure_ticks) return;
    uint32_t start = source->start_tick;
    uint32_t remaining = source->duration_ticks;
    bool has_previous_segment = false;
    bool crosses_bar = start / measure_ticks !=
                       (start + remaining - 1) / measure_ticks;
    while (remaining && out->note_count < MAX_NOTES) {
        uint32_t next_bar = (start / measure_ticks + 1) * measure_ticks;
        uint32_t available = next_bar > start ? next_bar - start : remaining;
        uint32_t duration = remaining < available ? remaining : available;
        bool has_next_segment = remaining > duration;
        midi_note_t *target = &out->notes[out->note_count++];
        *target = (midi_note_t){
            .note = source->pitch,
            .velocity = source->velocity,
            .staff = source->staff,
            .voice = source->voice,
            .dots = crosses_bar ? dots_for_exact_ticks(duration) : source->dots,
            .tie_flags = (uint8_t)((has_previous_segment ? MIDI_NOTE_TIE_STOP : 0) |
                                   (has_next_segment ? MIDI_NOTE_TIE_START : 0)),
            .slur_start = has_previous_segment ? 0 : source->slur_start,
            .slur_stop = has_next_segment ? 0 : source->slur_stop,
            .gliss_start = has_previous_segment ? 0 : source->gliss_start,
            .gliss_stop = has_next_segment ? 0 : source->gliss_stop,
            .start_tick = start,
            .duration = duration,
        };
        start += duration;
        remaining -= duration;
        has_previous_segment = true;
    }
}

esp_err_t creator_recorder_init(void)
{
    if (s_recorder.mutex) return ESP_OK;
    s_recorder.mutex = xSemaphoreCreateMutex();
    return s_recorder.mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

void creator_recorder_begin(const creator_recorder_config_t *config,
                            uint64_t timestamp_us)
{
    lock_recorder();
    s_recorder.config = normalized_config(config);
    s_recorder.note_count = 0;
    memset(s_recorder.notes, 0, sizeof(s_recorder.notes));
    memset(s_recorder.active, 0, sizeof(s_recorder.active));
    memset(s_recorder.sustain_pedal, 0, sizeof(s_recorder.sustain_pedal));
    s_recorder.raw_event_count = 0;
    s_recorder.raw_event_start = 0;
    reset_input_filter_locked();
    s_recorder.segment_origin_us = timestamp_us;
    s_recorder.segment_start_tick = 0;
    s_recorder.append_tick = 0;
    s_recorder.segment_started = false;
    s_recorder.range_recording = false;
    s_recorder.range_end_tick = 0;
    s_recorder.state = CREATOR_RECORDER_RECORDING;
    unlock_recorder();
}

void creator_recorder_stop(void)
{
    lock_recorder();
    memset(s_recorder.active, 0, sizeof(s_recorder.active));
    memset(s_recorder.sustain_pedal, 0, sizeof(s_recorder.sustain_pedal));
    reset_input_filter_locked();
    s_recorder.segment_started = false;
    s_recorder.range_recording = false;
    s_recorder.range_end_tick = 0;
    s_recorder.state = CREATOR_RECORDER_IDLE;
    unlock_recorder();
}

void creator_recorder_pause(uint64_t timestamp_us)
{
    lock_recorder();
    if (s_recorder.state == CREATOR_RECORDER_RECORDING) {
        enter_paused_locked(timestamp_us, false);
    }
    unlock_recorder();
}

void creator_recorder_resume(uint64_t timestamp_us)
{
    lock_recorder();
    if (s_recorder.state == CREATOR_RECORDER_PAUSED) {
        s_recorder.segment_start_tick =
            creator_quantize_onset_tick(s_recorder.append_tick);
        s_recorder.segment_origin_us = timestamp_us;
        s_recorder.segment_started = false;
        s_recorder.range_recording = false;
        s_recorder.range_end_tick = 0;
        reset_input_filter_locked();
        s_recorder.state = CREATOR_RECORDER_RECORDING;
    }
    unlock_recorder();
}

bool creator_recorder_resume_from_measure(int measure_number,
                                          uint64_t timestamp_us)
{
    if (measure_number < 1) return false;
    lock_recorder();
    if (s_recorder.state != CREATOR_RECORDER_PAUSED) {
        unlock_recorder();
        return false;
    }

    uint32_t boundary = (uint32_t)(measure_number - 1) *
                        measure_ticks_locked();
    int write_index = 0;
    for (int i = 0; i < s_recorder.note_count; ++i) {
        creator_recorded_note_t note = s_recorder.notes[i];
        if (note.start_tick >= boundary) continue;
        uint32_t end_tick = note.start_tick + note.duration_ticks;
        if (end_tick > boundary) {
            note.duration_ticks = boundary - note.start_tick;
            note.dots = dots_for_exact_ticks(note.duration_ticks);
        }
        if (note.duration_ticks)
            s_recorder.notes[write_index++] = note;
    }
    s_recorder.note_count = write_index;
    remove_dangling_connections_locked();
    memset(s_recorder.active, 0, sizeof(s_recorder.active));
    s_recorder.append_tick = boundary;
    s_recorder.segment_start_tick = boundary;
    s_recorder.segment_origin_us = timestamp_us;
    s_recorder.segment_started = false;
    s_recorder.range_recording = false;
    s_recorder.range_end_tick = 0;
    reset_input_filter_locked();
    s_recorder.state = CREATOR_RECORDER_RECORDING;
    unlock_recorder();
    return true;
}

bool creator_recorder_resume_measure_range(int first_measure,
                                           int last_measure,
                                           uint64_t timestamp_us)
{
    if (first_measure < 1 || last_measure < 1) return false;
    if (first_measure > last_measure) {
        int swap = first_measure;
        first_measure = last_measure;
        last_measure = swap;
    }

    lock_recorder();
    if (s_recorder.state != CREATOR_RECORDER_PAUSED) {
        unlock_recorder();
        return false;
    }
    uint32_t measure_ticks = measure_ticks_locked();
    uint32_t range_start = (uint32_t)(first_measure - 1) * measure_ticks;
    uint32_t range_end = (uint32_t)last_measure * measure_ticks;

    int write_index = 0;
    for (int i = 0; i < s_recorder.note_count; ++i) {
        creator_recorded_note_t note = s_recorder.notes[i];
        if (note.start_tick >= range_start && note.start_tick < range_end)
            continue;
        if (note.start_tick < range_start) {
            uint32_t end_tick = note.start_tick + note.duration_ticks;
            if (end_tick > range_start)
                note.duration_ticks = range_start - note.start_tick;
            note.dots = dots_for_exact_ticks(note.duration_ticks);
            if (!note.duration_ticks) continue;
        }
        s_recorder.notes[write_index++] = note;
    }
    s_recorder.note_count = write_index;
    remove_dangling_connections_locked();
    memset(s_recorder.active, 0, sizeof(s_recorder.active));
    recompute_append_tick_locked();
    s_recorder.segment_start_tick = range_start;
    s_recorder.segment_origin_us = timestamp_us;
    s_recorder.segment_started = false;
    s_recorder.range_recording = true;
    s_recorder.range_end_tick = range_end;
    reset_input_filter_locked();
    s_recorder.state = CREATOR_RECORDER_RECORDING;
    unlock_recorder();
    return true;
}

bool creator_recorder_note_on(uint8_t channel, uint8_t pitch, uint8_t velocity,
                              uint64_t timestamp_us)
{
    if (pitch > 127 || velocity == 0) return false;
    lock_recorder();
    if (s_recorder.state != CREATOR_RECORDER_RECORDING) {
        unlock_recorder();
        return false;
    }
    if (!s_recorder.segment_started) {
        s_recorder.segment_origin_us = timestamp_us;
        s_recorder.segment_started = true;
    }
    if (range_end_reached_locked(timestamp_us)) {
        enter_paused_locked(timestamp_us, true);
        unlock_recorder();
        return false;
    }

    uint32_t start_tick;
    if (!accept_note_on_locked(pitch, timestamp_us, &start_tick)) {
        uint8_t *suppressed =
            &s_recorder.suppressed_note_on[channel & 0x0f][pitch];
        if (*suppressed != UINT8_MAX) (*suppressed)++;
        unlock_recorder();
        return false;
    }

    active_note_t *slot = NULL;
    for (int i = 0; i < CREATOR_MAX_ACTIVE_NOTES; ++i) {
        active_note_t *candidate = &s_recorder.active[i];
        if (candidate->used && candidate->channel == channel &&
            candidate->pitch == pitch) {
            finish_active_locked(candidate, timestamp_us);
            slot = candidate;
            break;
        }
        if (!candidate->used && !slot) slot = candidate;
    }
    if (!slot) {
        unlock_recorder();
        return false;
    }

    *slot = (active_note_t){
        .used = true,
        .key_released = false,
        .channel = channel,
        .pitch = pitch,
        .velocity = velocity,
        .start_time_us = timestamp_us,
        .start_tick = start_tick,
    };
    unlock_recorder();
    return true;
}

bool creator_recorder_note_off(uint8_t channel, uint8_t pitch,
                               uint64_t timestamp_us)
{
    if (pitch > 127) return false;
    lock_recorder();
    if (s_recorder.state != CREATOR_RECORDER_RECORDING) {
        unlock_recorder();
        return false;
    }
    if (range_end_reached_locked(timestamp_us)) {
        enter_paused_locked(timestamp_us, true);
        unlock_recorder();
        return false;
    }
    uint8_t *suppressed =
        &s_recorder.suppressed_note_on[channel & 0x0f][pitch];
    if (*suppressed) {
        (*suppressed)--;
        unlock_recorder();
        return false;
    }
    bool finished = false;
    for (int i = 0; i < CREATOR_MAX_ACTIVE_NOTES; ++i) {
        active_note_t *active = &s_recorder.active[i];
        if (active->used && active->channel == channel &&
            active->pitch == pitch) {
            if (s_recorder.sustain_pedal[channel & 0x0f]) {
                active->key_released = true;
                finished = true;
            } else {
                finished = finish_active_locked(active, timestamp_us);
            }
            break;
        }
    }
    if (finished && s_recorder.range_recording && s_recorder.note_count > 0) {
        const creator_recorded_note_t *last =
            &s_recorder.notes[s_recorder.note_count - 1];
        if (last->start_tick + last->duration_ticks >=
            s_recorder.range_end_tick)
            enter_paused_locked(timestamp_us, true);
    }
    unlock_recorder();
    return finished;
}

bool creator_recorder_control_change(uint8_t channel, uint8_t controller,
                                     uint8_t value, uint64_t timestamp_us)
{
    if (controller > 127 || value > 127) return false;
    lock_recorder();
    if (s_recorder.state != CREATOR_RECORDER_RECORDING) {
        unlock_recorder();
        return false;
    }
    channel &= 0x0f;
    retain_raw_event_locked(MIDI_RAW_CONTROL_CHANGE, channel, controller,
                            value, timestamp_us);
    if (controller == 64) {
        bool was_down = s_recorder.sustain_pedal[channel];
        bool is_down = value >= 64;
        s_recorder.sustain_pedal[channel] = is_down;
        if (was_down && !is_down) {
            for (int i = 0; i < CREATOR_MAX_ACTIVE_NOTES; ++i) {
                active_note_t *active = &s_recorder.active[i];
                if (active->used && active->channel == channel &&
                    active->key_released)
                    finish_active_locked(active, timestamp_us);
            }
        }
    }
    unlock_recorder();
    return true;
}

bool creator_recorder_pitch_bend(uint8_t channel, int16_t value,
                                 uint64_t timestamp_us)
{
    if (value < -8192) value = -8192;
    if (value > 8191) value = 8191;
    lock_recorder();
    if (s_recorder.state != CREATOR_RECORDER_RECORDING) {
        unlock_recorder();
        return false;
    }
    retain_raw_event_locked(MIDI_RAW_PITCH_BEND, channel, 0, value,
                            timestamp_us);
    unlock_recorder();
    return true;
}

bool creator_recorder_add_connection(creator_connection_kind_t kind,
                                     int start_note_index,
                                     int end_note_index,
                                     uint8_t number)
{
    if (!number || start_note_index < 0 || end_note_index <= start_note_index)
        return false;
    lock_recorder();
    if (end_note_index >= s_recorder.note_count) {
        unlock_recorder();
        return false;
    }
    creator_recorded_note_t *start = &s_recorder.notes[start_note_index];
    creator_recorded_note_t *end = &s_recorder.notes[end_note_index];
    if (kind == CREATOR_CONNECTION_SLUR) {
        start->slur_start = number;
        end->slur_stop = number;
    } else if (kind == CREATOR_CONNECTION_GLISS) {
        start->gliss_start = number;
        end->gliss_stop = number;
    } else {
        unlock_recorder();
        return false;
    }
    unlock_recorder();
    return true;
}

creator_recorder_state_t creator_recorder_state(void)
{
    lock_recorder();
    creator_recorder_state_t state = s_recorder.state;
    unlock_recorder();
    return state;
}

bool creator_recorder_waiting_for_first_note(void)
{
    lock_recorder();
    bool waiting = s_recorder.state == CREATOR_RECORDER_RECORDING &&
                   !s_recorder.segment_started;
    unlock_recorder();
    return waiting;
}

bool creator_recorder_poll(uint64_t timestamp_us)
{
    lock_recorder();
    bool completed = s_recorder.state == CREATOR_RECORDER_RECORDING &&
                     range_end_reached_locked(timestamp_us);
    if (completed)
        enter_paused_locked(timestamp_us, true);
    unlock_recorder();
    return completed;
}

int creator_recorder_note_count(void)
{
    lock_recorder();
    int count = s_recorder.note_count;
    unlock_recorder();
    return count;
}

uint32_t creator_recorder_measure_ticks(void)
{
    lock_recorder();
    uint32_t ticks = measure_ticks_locked();
    unlock_recorder();
    return ticks;
}

int creator_recorder_measure_count(void)
{
    lock_recorder();
    uint32_t measure_ticks = measure_ticks_locked();
    uint32_t end_tick = s_recorder.append_tick;
    int result = measure_ticks ?
        (int)((end_tick + measure_ticks - 1) / measure_ticks) : 1;
    if (result < 1) result = 1;
    unlock_recorder();
    return result;
}

bool creator_recorder_get_note(int index, creator_recorded_note_t *out)
{
    if (!out) return false;
    lock_recorder();
    bool valid = index >= 0 && index < s_recorder.note_count;
    if (valid) *out = s_recorder.notes[index];
    unlock_recorder();
    return valid;
}

static bool snapshot_locked(midi_data_t *out, uint64_t timestamp_us,
                            bool include_active)
{
    memset(out, 0, sizeof(*out));
    out->bpm = s_recorder.config.bpm;
    out->ticks_per_quarter = CREATOR_TICKS_PER_QUARTER;
    out->time_sig_num = s_recorder.config.time_sig_num;
    out->time_sig_den = s_recorder.config.time_sig_den == 8 ? 3 : 2;
    out->tonality_sf = 0;
    out->tonality_minor = false;
    snprintf(out->title, sizeof(out->title), "Creator Mode");
    out->note_count = 0;
    uint32_t measure_ticks = measure_ticks_locked();
    for (int i = 0; i < s_recorder.note_count && i < MAX_NOTES; ++i) {
        const creator_recorded_note_t *source = &s_recorder.notes[i];
        append_split_note_locked(out, source, measure_ticks);
    }
    if (include_active) {
        for (int i = 0; i < CREATOR_MAX_ACTIVE_NOTES &&
                        out->note_count < MAX_NOTES; ++i) {
            const active_note_t *active = &s_recorder.active[i];
            if (!active->used) continue;
            uint64_t duration_us = timestamp_us > active->start_time_us ?
                                   timestamp_us - active->start_time_us : 1;
            creator_quantized_duration_t duration =
                creator_quantize_duration(
                    duration_us, s_recorder.config.bpm,
                    s_recorder.config.duration_tolerance_percent);
            uint32_t duration_ticks = duration.ticks;
            bool duration_clipped = s_recorder.range_recording &&
                active->start_tick < s_recorder.range_end_tick &&
                duration_ticks > s_recorder.range_end_tick - active->start_tick;
            if (duration_clipped)
                duration_ticks = s_recorder.range_end_tick - active->start_tick;
            uint8_t staff;
            uint8_t voice;
            assign_staff_locked(active->pitch, &staff, &voice);
            creator_recorded_note_t live = {
                .pitch = active->pitch,
                .velocity = active->velocity,
                .staff = staff,
                .voice = voice,
                .start_tick = active->start_tick,
                .duration_ticks = duration_ticks,
                .note_value = duration.value,
                .dots = duration_clipped ?
                        dots_for_exact_ticks(duration_ticks) : duration.dots,
            };
            append_split_note_locked(out, &live, measure_ticks);
        }
    }
    out->raw_event_count = s_recorder.raw_event_count;
    if (out->raw_event_count > MAX_MIDI_RAW_EVENTS)
        out->raw_event_count = MAX_MIDI_RAW_EVENTS;
    for (int i = 0; i < out->raw_event_count; ++i) {
        int source_index = (s_recorder.raw_event_start + i) %
                           MAX_MIDI_RAW_EVENTS;
        out->raw_events[i] = s_recorder.raw_events[source_index];
    }
    qsort(out->notes, (size_t)out->note_count, sizeof(out->notes[0]),
          compare_midi_notes);
    return true;
}

bool creator_recorder_snapshot(midi_data_t *out)
{
    if (!out) return false;
    lock_recorder();
    bool result = snapshot_locked(out, 0, false);
    unlock_recorder();
    return result;
}

bool creator_recorder_snapshot_live(midi_data_t *out,
                                    uint64_t timestamp_us)
{
    if (!out) return false;
    lock_recorder();
    bool result = snapshot_locked(out, timestamp_us, true);
    unlock_recorder();
    return result;
}
