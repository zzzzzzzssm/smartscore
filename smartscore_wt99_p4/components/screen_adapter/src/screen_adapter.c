#include "screen_adapter.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_font.h"
#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "creator_mode.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "events_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gui_guider.h"
#include "input_source_manager.h"
#include "music_display.h"
#include "score_data.h"
#include "score_storage.h"
#include "score_ui_flow.h"
#include "scoring_service.h"
#include "s3_devices.h"
#include "screen_modes.h"
#include "screen_score_bridge.h"
#include "speaker_service.h"
#include "usb_midi_input.h"

#define SCREEN_INIT_TASK_STACK_BYTES 16384
#define SCREEN_RESULT_TASK_STACK_BYTES 6144
#define SCREEN_TASK_PRIORITY 4
#define SCREEN_FOLLOW_TASK_STACK_BYTES 6144
#define SCREEN_FOLLOW_TASK_PRIORITY 3
#define SCREEN_FOLLOW_TASK_PERIOD_MS 50
#define SCREEN_PAGE_CHECK_PERIOD_MS 125
#define SCREEN_AUDIO_COUNTDOWN_TASK_STACK_BYTES 4096
#define SCREEN_AUDIO_COUNTDOWN_SECONDS 3
#define SCREEN_RESULT_TIMEOUT_MS 15000
#define SCREEN_VOICE_LISTEN_TIMEOUT_MS 6500
#define SCREEN_VOICE_FEEDBACK_TIMEOUT_MS 1800
#define SCREEN_RHYTHM_TOLERANCE_MS 300
#define SCREEN_MATCH_WINDOW_SIZE 6
#define SCREEN_MATCH_PITCH_BONUS 10000
#define SCREEN_MATCH_ONSET_WEIGHT 6
#define SCREEN_FOLLOW_MATCH_PITCH_TOLERANCE 5
#define SCREEN_AUDIO_FIRST_NOTE_PITCH_TOLERANCE 2
#define SCREEN_DISPLAY_GREEN_PITCH_TOLERANCE 2
#define SCREEN_TARGET_FINAL_GRACE_MS 80
#define SCREEN_SESSION_FINISH_GRACE_MS 600
#define SCREEN_MISSING_BATCH_CAPACITY 8
#define SCREEN_PENDING_NOTE_CAPACITY 64
#define SCREEN_RECENT_MATCH_CAPACITY 8
#define SCREEN_RECENT_EXTRA_CAPACITY 64
#define SCREEN_DURATION_TOLERANCE_RATIO 0.40f
#define SCREEN_TEMPO_EMA_ALPHA 0.15f
#define SCREEN_AUDIO_TEMPO_EMA_ALPHA 0.05f
#define SCREEN_AUDIO_PHASE_CORRECTION_ALPHA 0.10f
#define SCREEN_AUDIO_PHASE_CORRECTION_MAX_US 25000LL
#define SCREEN_TEMPO_MIN 0.50f
#define SCREEN_TEMPO_MAX 2.00f

typedef struct {
    bool active;
    size_t target_index;
    uint8_t expected_note;
    uint8_t played_note;
    uint8_t channel;
    int64_t played_onset_us;
    int64_t target_onset_us;
    int64_t target_duration_us;
    bool pitch_ok;
    bool onset_ok;
} pending_note_t;

typedef struct {
    bool valid;
    int64_t played_onset_us;
    int64_t target_onset_us;
} recent_match_t;

typedef struct {
    size_t target_index;
    uint8_t expected_note;
    int16_t played_note;
    bool pitch_ok;
} settled_note_result_t;

typedef struct {
    bool valid;
    bool consumed;
    uint8_t midi;
    int64_t score_time_ms;
} extra_note_observation_t;

typedef struct {
    uint32_t generation;
    bool metronome_enabled;
    uint16_t metronome_bpm;
    uint8_t time_sig_num;
    uint8_t time_sig_den;
} audio_countdown_context_t;

typedef enum {
    RESULT_ACTION_SHOW = 0,
    RESULT_ACTION_RESET,
    RESULT_ACTION_RESTART,
} result_action_t;

typedef struct {
    screen_voice_event_type_t type;
    uint8_t command_id;
    screen_voice_command_result_cb_t result_callback;
    void *result_context;
} voice_ui_event_t;

LV_FONT_DECLARE(lv_font_gudianChinese_34);
LV_FONT_DECLARE(lv_font_gudianChinese_34_extra);

lv_ui guider_ui;

static const char *TAG = "SCREEN_ADAPTER";
static SemaphoreHandle_t s_session_lock;
static score_document_t s_target_score;
static size_t s_aligned_index;
static int64_t s_performance_origin_us;
static float s_tempo_ema = 1.0f;
static bool s_target_consumed[SCORE_DATA_MAX_NOTES];
static pending_note_t s_pending_notes[SCREEN_PENDING_NOTE_CAPACITY];
static recent_match_t s_recent_matches[SCREEN_RECENT_MATCH_CAPACITY];
static extra_note_observation_t
    s_extra_observations[SCREEN_RECENT_EXTRA_CAPACITY];
static size_t s_recent_match_head;
static size_t s_recent_match_count;
static size_t s_extra_observation_head;
static uint32_t s_score_end_ms;
static bool s_auto_finish_queued;
static bool s_session_active;
static bool s_session_paused;
static input_source_t s_session_input = INPUT_SOURCE_NONE;
static bool s_first_match_pending;
static int64_t s_pause_started_us;
static bool s_metronome_running;
static uint32_t s_audio_countdown_generation;
static volatile bool s_ready;
static volatile bool s_start_requested;
static bool s_practice_starting;
static volatile bool s_result_task_running;
static volatile result_action_t s_result_action = RESULT_ACTION_SHOW;
static lv_font_t s_other_mode_font;
static lv_font_t s_audio_countdown_font;
static lv_obj_t *s_stop_button;
static lv_obj_t *s_pause_button;
static lv_obj_t *s_pause_label;
static lv_obj_t *s_audio_countdown_overlay;
static lv_obj_t *s_audio_countdown_label;
static lv_obj_t *s_voice_popup;
static lv_obj_t *s_voice_popup_accent;
static lv_obj_t *s_voice_popup_label;
static lv_timer_t *s_voice_popup_timer;
static lv_obj_t *s_result_back_button;
static lv_obj_t *s_watched_volume_slider;
static lv_timer_t *s_ui_watch_timer;
static bool s_applying_volume_status;
static screen_preparation_status_t s_preparation;
static char *s_preparation_json;
static size_t s_preparation_json_length;
static void hide_practice_buttons(void);
static void finish_practice(void);
static void finish_practice_async_cb(void *user_data);
static void queue_automatic_finish(void);
static void hide_audio_countdown_overlay(void);
static bool begin_practice_completion(result_action_t action);
static bool begin_practice_completion_internal(result_action_t action,
                                                bool allow_detached_recording);

static void notify_camera_practice_state(
    s3_camera_practice_state_t state)
{
    esp_err_t err = s3_camera_node_send_practice_state(state);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "unable to synchronize camera practice state: %s",
                 esp_err_to_name(err));
    }
}

static void set_music_status(const char *text)
{
    bsp_display_lock(portMAX_DELAY);
    if (guider_ui.music_screen_status_label &&
        lv_obj_is_valid(guider_ui.music_screen_status_label)) {
        lv_label_set_text(guider_ui.music_screen_status_label,
                          text != NULL ? text : "");
    }
    bsp_display_unlock();
}

static void hide_voice_popup(void)
{
    if (s_voice_popup_timer != NULL) {
        lv_timer_delete(s_voice_popup_timer);
        s_voice_popup_timer = NULL;
    }
    if (s_voice_popup && lv_obj_is_valid(s_voice_popup)) {
        lv_obj_add_flag(s_voice_popup, LV_OBJ_FLAG_HIDDEN);
    }
}

static void voice_popup_timeout_cb(lv_timer_t *timer)
{
    if (timer == s_voice_popup_timer) s_voice_popup_timer = NULL;
    if (s_voice_popup && lv_obj_is_valid(s_voice_popup)) {
        lv_obj_add_flag(s_voice_popup, LV_OBJ_FLAG_HIDDEN);
    }
    lv_timer_delete(timer);
}

static void ensure_voice_popup(void)
{
    if (s_voice_popup && lv_obj_is_valid(s_voice_popup)) return;

    s_voice_popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_voice_popup, 430, 132);
    lv_obj_align(s_voice_popup, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_radius(s_voice_popup, 26, 0);
    lv_obj_set_style_bg_color(s_voice_popup, lv_color_hex(0x172033), 0);
    lv_obj_set_style_bg_opa(s_voice_popup, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_voice_popup, 1, 0);
    lv_obj_set_style_border_color(s_voice_popup, lv_color_hex(0x7DD3FC), 0);
    lv_obj_set_style_shadow_width(s_voice_popup, 24, 0);
    lv_obj_set_style_shadow_opa(s_voice_popup, LV_OPA_30, 0);
    lv_obj_set_style_shadow_color(s_voice_popup, lv_color_hex(0x000000), 0);
    lv_obj_clear_flag(s_voice_popup, LV_OBJ_FLAG_SCROLLABLE);

    s_voice_popup_accent = lv_obj_create(s_voice_popup);
    lv_obj_set_size(s_voice_popup_accent, 54, 54);
    lv_obj_align(s_voice_popup_accent, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_style_radius(s_voice_popup_accent, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_voice_popup_accent, 0, 0);
    lv_obj_set_style_bg_color(s_voice_popup_accent,
                              lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_bg_opa(s_voice_popup_accent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_voice_popup_accent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *symbol = lv_label_create(s_voice_popup_accent);
    lv_label_set_text(symbol, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(symbol, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(symbol);

    s_voice_popup_label = lv_label_create(s_voice_popup);
    lv_obj_set_pos(s_voice_popup_label, 92, 22);
    lv_obj_set_size(s_voice_popup_label, 312, 88);
    const lv_font_t *font = app_font_chinese_22();
    if (font) lv_obj_set_style_text_font(s_voice_popup_label, font, 0);
    lv_obj_set_style_text_color(s_voice_popup_label,
                                lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(s_voice_popup_label,
                                LV_TEXT_ALIGN_LEFT, 0);
}

static void show_voice_popup(const char *text, bool handled,
                             uint32_t timeout_ms)
{
    ensure_voice_popup();
    if (!s_voice_popup || !lv_obj_is_valid(s_voice_popup)) return;

    lv_obj_set_style_bg_color(
        s_voice_popup_accent,
        lv_color_hex(handled ? 0x38BDF8 : 0xF59E0B), 0);
    lv_label_set_text(s_voice_popup_label, text ? text : "");
    lv_obj_clear_flag(s_voice_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(
        s_voice_popup, lv_obj_get_child_count(lv_layer_top()) - 1);

    if (s_voice_popup_timer != NULL) {
        lv_timer_delete(s_voice_popup_timer);
    }
    s_voice_popup_timer = lv_timer_create(
        voice_popup_timeout_cb, timeout_ms, NULL);
    if (s_voice_popup_timer != NULL) {
        lv_timer_set_repeat_count(s_voice_popup_timer, 1);
    }
}

static void show_audio_countdown_overlay(int seconds)
{
    char text[32];
    snprintf(text, sizeof(text),
             "\xE5\x87\x86\xE5\xA4\x87\xE6\xBC\x94\xE5\xA5\x8F\n%d",
             seconds);

    bsp_display_lock(portMAX_DELAY);
    if (!guider_ui.music_screen ||
        !lv_obj_is_valid(guider_ui.music_screen)) {
        bsp_display_unlock();
        return;
    }
    if (!s_audio_countdown_overlay ||
        !lv_obj_is_valid(s_audio_countdown_overlay)) {
        s_audio_countdown_overlay =
            lv_obj_create(guider_ui.music_screen);
        lv_obj_set_size(s_audio_countdown_overlay, 220, 170);
        lv_obj_center(s_audio_countdown_overlay);
        lv_obj_set_style_radius(s_audio_countdown_overlay, 18, 0);
        lv_obj_set_style_bg_color(s_audio_countdown_overlay,
                                  lv_color_hex(0x18233A), 0);
        lv_obj_set_style_bg_opa(s_audio_countdown_overlay,
                                LV_OPA_90, 0);
        lv_obj_set_style_border_width(s_audio_countdown_overlay, 0, 0);
        lv_obj_clear_flag(s_audio_countdown_overlay,
                          LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_audio_countdown_overlay,
                        LV_OBJ_FLAG_CLICKABLE);

        s_audio_countdown_label =
            lv_label_create(s_audio_countdown_overlay);
        s_audio_countdown_font = lv_font_gudianChinese_34;
        s_audio_countdown_font.fallback =
            &lv_font_gudianChinese_34_extra;
        lv_obj_set_style_text_font(s_audio_countdown_label,
                                   &s_audio_countdown_font, 0);
        lv_obj_set_style_text_color(s_audio_countdown_label,
                                    lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_align(s_audio_countdown_label,
                                    LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_audio_countdown_label);
    }
    lv_label_set_text(s_audio_countdown_label, text);
    lv_obj_clear_flag(s_audio_countdown_overlay, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

static void hide_audio_countdown_overlay(void)
{
    bsp_display_lock(portMAX_DELAY);
    if (s_audio_countdown_overlay &&
        lv_obj_is_valid(s_audio_countdown_overlay)) {
        lv_obj_add_flag(s_audio_countdown_overlay,
                        LV_OBJ_FLAG_HIDDEN);
    }
    bsp_display_unlock();
}

static float clamp_tempo(float tempo)
{
    if (tempo < SCREEN_TEMPO_MIN) return SCREEN_TEMPO_MIN;
    if (tempo > SCREEN_TEMPO_MAX) return SCREEN_TEMPO_MAX;
    return tempo;
}

static int64_t scaled_target_time_us(int64_t target_time_us)
{
    float tempo = s_tempo_ema > 0.01f ? s_tempo_ema : 1.0f;
    return (int64_t)((double)target_time_us / (double)tempo);
}

static void reset_alignment_locked(void)
{
    s_aligned_index = 0;
    s_performance_origin_us = 0;
    s_tempo_ema = 1.0f;
    s_recent_match_head = 0;
    s_recent_match_count = 0;
    s_extra_observation_head = 0;
    s_score_end_ms = 0;
    s_auto_finish_queued = false;
    s_first_match_pending = false;
    memset(s_target_consumed, 0, sizeof(s_target_consumed));
    memset(s_pending_notes, 0, sizeof(s_pending_notes));
    memset(s_recent_matches, 0, sizeof(s_recent_matches));
    memset(s_extra_observations, 0, sizeof(s_extra_observations));
}

static int midi_distance(uint8_t left, uint8_t right)
{
    int distance = (int)left - (int)right;
    return distance < 0 ? -distance : distance;
}

/*
 * Search only the next onset group inside a six-note window. Piano chord
 * notes may arrive in any order, while later same-pitch notes cannot steal
 * the current match.
 */
static int find_best_match_in_window(const score_note_t *notes,
                                     size_t note_count,
                                     int start_index,
                                     int window_size,
                                     uint8_t midi_note,
                                     int64_t timestamp_us,
                                     int64_t expected_us,
                                     int pitch_tolerance,
                                     int *out_score)
{
    if (out_score) *out_score = 0;
    if (!notes || note_count == 0 || start_index < 0 ||
        (size_t)start_index >= note_count || window_size <= 0) {
        return -1;
    }

    size_t end = (size_t)start_index + (size_t)window_size;
    if (end > note_count) end = note_count;
    int best_index = -1;
    int best_score = 0;
    int64_t start_target_us =
        (int64_t)notes[start_index].start_ms * 1000LL;
    uint32_t active_onset_ms = UINT32_MAX;

    for (size_t index = (size_t)start_index; index < end; ++index) {
        if (s_target_consumed[index]) continue;

        int64_t onset_error_us = 0;
        if (s_performance_origin_us == 0) {
            if (notes[index].start_ms != notes[start_index].start_ms) break;
            active_onset_ms = notes[start_index].start_ms;
        } else {
            int64_t target_delta_us =
                (int64_t)notes[index].start_ms * 1000LL -
                start_target_us;
            int64_t candidate_expected_us =
                expected_us + scaled_target_time_us(target_delta_us);
            int64_t rhythm_tolerance_us =
                (int64_t)SCREEN_RHYTHM_TOLERANCE_MS * 1000LL;
            if (timestamp_us < candidate_expected_us - rhythm_tolerance_us) {
                break;
            }
            if (timestamp_us > candidate_expected_us + rhythm_tolerance_us) {
                continue;
            }
            if (active_onset_ms == UINT32_MAX) {
                active_onset_ms = notes[index].start_ms;
            } else if (notes[index].start_ms != active_onset_ms) {
                break;
            }
            onset_error_us = timestamp_us - candidate_expected_us;
            if (onset_error_us < 0) onset_error_us = -onset_error_us;
        }

        int pitch_distance = midi_distance(notes[index].midi, midi_note);
        if (pitch_distance > pitch_tolerance) {
            continue;
        }
        int score = SCREEN_MATCH_PITCH_BONUS -
                    pitch_distance * 1000 -
                    (int)(onset_error_us / 1000LL) *
                        SCREEN_MATCH_ONSET_WEIGHT;
        if (best_index < 0 || score > best_score) {
            best_index = (int)index;
            best_score = score;
        }
    }

    if (out_score) *out_score = best_score;
    return best_index;
}

static void remember_extra_observation_locked(uint8_t midi,
                                              int64_t score_time_ms)
{
    if (score_time_ms < 0) return;
    s_extra_observations[s_extra_observation_head] =
        (extra_note_observation_t) {
            .valid = true,
            .consumed = false,
            .midi = midi,
            .score_time_ms = score_time_ms,
        };
    s_extra_observation_head =
        (s_extra_observation_head + 1U) %
        SCREEN_RECENT_EXTRA_CAPACITY;
}

static settled_note_result_t settle_unplayed_target_locked(
    size_t target_index)
{
    const score_note_t *target = &s_target_score.notes[target_index];
    settled_note_result_t result = {
        .target_index = target_index,
        .expected_note = target->midi,
        .played_note = -1,
        .pitch_ok = false,
    };
    int64_t window_start_ms =
        target->start_ms > SCREEN_RHYTHM_TOLERANCE_MS
            ? (int64_t)target->start_ms - SCREEN_RHYTHM_TOLERANCE_MS
            : 0;
    int64_t window_end_ms =
        (int64_t)target->start_ms +
        (int64_t)target->duration_ms +
        SCREEN_TARGET_FINAL_GRACE_MS;
    int best_index = -1;
    int best_pitch_distance = INT32_MAX;
    int64_t best_time_distance = INT64_MAX;

    for (size_t index = 0;
         index < SCREEN_RECENT_EXTRA_CAPACITY; ++index) {
        const extra_note_observation_t *observation =
            &s_extra_observations[index];
        if (!observation->valid || observation->consumed ||
            observation->score_time_ms < window_start_ms ||
            observation->score_time_ms > window_end_ms) {
            continue;
        }
        int pitch_distance =
            midi_distance(target->midi, observation->midi);
        int64_t time_distance =
            observation->score_time_ms - (int64_t)target->start_ms;
        if (time_distance < 0) time_distance = -time_distance;
        if (best_index < 0 ||
            pitch_distance < best_pitch_distance ||
            (pitch_distance == best_pitch_distance &&
             time_distance < best_time_distance)) {
            best_index = (int)index;
            best_pitch_distance = pitch_distance;
            best_time_distance = time_distance;
        }
    }

    if (best_index >= 0) {
        extra_note_observation_t *observation =
            &s_extra_observations[best_index];
        observation->consumed = true;
        result.played_note = observation->midi;
        result.pitch_ok =
            best_pitch_distance <=
            SCREEN_DISPLAY_GREEN_PITCH_TOLERANCE;
    }
    return result;
}

static size_t consume_matched_target_locked(
    size_t target_index,
    settled_note_result_t *settled,
    size_t settled_capacity)
{
    if (target_index >= s_target_score.note_count ||
        target_index >= SCORE_DATA_MAX_NOTES) {
        return 0;
    }

    size_t settled_count = 0;
    uint32_t matched_start = s_target_score.notes[target_index].start_ms;
    for (size_t index = s_aligned_index; index < target_index; ++index) {
        if (s_target_score.notes[index].start_ms < matched_start) {
            s_target_consumed[index] = true;
            if (settled && settled_count < settled_capacity) {
                settled[settled_count++] =
                    settle_unplayed_target_locked(index);
            }
        }
    }
    s_target_consumed[target_index] = true;
    while (s_aligned_index < s_target_score.note_count &&
           s_aligned_index < SCORE_DATA_MAX_NOTES &&
           s_target_consumed[s_aligned_index]) {
        ++s_aligned_index;
    }
    return settled_count;
}

static bool add_pending_note_locked(size_t target_index,
                                    const usb_midi_event_t *event,
                                    bool pitch_ok,
                                    bool onset_ok)
{
    for (size_t index = 0; index < SCREEN_PENDING_NOTE_CAPACITY; ++index) {
        pending_note_t *pending = &s_pending_notes[index];
        if (pending->active) continue;
        const score_note_t *target = &s_target_score.notes[target_index];
        *pending = (pending_note_t) {
            .active = true,
            .target_index = target_index,
            .expected_note = target->midi,
            .played_note = event->midi,
            .channel = event->channel,
            .played_onset_us = (int64_t)event->timestamp_us,
            .target_onset_us = (int64_t)target->start_ms * 1000LL,
            .target_duration_us =
                (int64_t)target->duration_ms * 1000LL,
            .pitch_ok = pitch_ok,
            .onset_ok = onset_ok,
        };
        return true;
    }
    ESP_LOGW(TAG, "pending MIDI note capacity reached");
    return false;
}

static int find_pending_note_locked(uint8_t midi_note, uint8_t channel)
{
    int best_index = -1;
    int64_t oldest_onset = INT64_MAX;
    for (size_t index = 0; index < SCREEN_PENDING_NOTE_CAPACITY; ++index) {
        const pending_note_t *pending = &s_pending_notes[index];
        if (!pending->active || pending->played_note != midi_note ||
            pending->channel != channel) {
            continue;
        }
        if (pending->played_onset_us < oldest_onset) {
            oldest_onset = pending->played_onset_us;
            best_index = (int)index;
        }
    }
    return best_index;
}

static void update_tempo_estimate(int64_t played_onset_us,
                                  int64_t target_onset_us,
                                  int64_t played_duration_us,
                                  int64_t target_duration_us)
{
    if (played_duration_us <= 0 || target_duration_us <= 0) return;

    float duration_speed =
        (float)target_duration_us / (float)played_duration_us;
    float sample = duration_speed;
    bool have_sample = duration_speed >= SCREEN_TEMPO_MIN &&
                       duration_speed <= SCREEN_TEMPO_MAX;
    if (s_recent_match_count > 0) {
        size_t latest_index =
            (s_recent_match_head + SCREEN_RECENT_MATCH_CAPACITY - 1U) %
            SCREEN_RECENT_MATCH_CAPACITY;
        const recent_match_t *previous = &s_recent_matches[latest_index];
        int64_t target_delta =
            target_onset_us - previous->target_onset_us;
        int64_t played_delta =
            played_onset_us - previous->played_onset_us;
        if (previous->valid && target_delta >= 50000LL &&
            played_delta > 0) {
            float onset_speed =
                (float)target_delta / (float)played_delta;
            if (onset_speed >= SCREEN_TEMPO_MIN &&
                onset_speed <= SCREEN_TEMPO_MAX) {
                sample = have_sample
                             ? 0.5f * (sample + onset_speed)
                             : onset_speed;
                have_sample = true;
            }
        }
    }
    if (!have_sample) return;

    sample = clamp_tempo(sample);
    const float tempo_alpha =
        s_session_input == INPUT_SOURCE_AUDIO_S3
            ? SCREEN_AUDIO_TEMPO_EMA_ALPHA
            : SCREEN_TEMPO_EMA_ALPHA;
    s_tempo_ema = clamp_tempo(
        (1.0f - tempo_alpha) * s_tempo_ema + tempo_alpha * sample);
    if (s_session_input == INPUT_SOURCE_AUDIO_S3 &&
        s_performance_origin_us > 0) {
        /* Audio pitch decisions arrive after a DSP stability window. Keep the
         * countdown-started timeline authoritative and correct its phase only
         * a few milliseconds per matched note, avoiding visible cursor jumps. */
        const int64_t expected_onset_us =
            s_performance_origin_us +
            scaled_target_time_us(target_onset_us);
        int64_t phase_error_us = played_onset_us - expected_onset_us;
        int64_t correction_us = (int64_t)(
            (double)phase_error_us *
            SCREEN_AUDIO_PHASE_CORRECTION_ALPHA);
        if (correction_us > SCREEN_AUDIO_PHASE_CORRECTION_MAX_US) {
            correction_us = SCREEN_AUDIO_PHASE_CORRECTION_MAX_US;
        } else if (correction_us <
                   -SCREEN_AUDIO_PHASE_CORRECTION_MAX_US) {
            correction_us = -SCREEN_AUDIO_PHASE_CORRECTION_MAX_US;
        }
        s_performance_origin_us += correction_us;
    } else {
        s_performance_origin_us =
            played_onset_us - scaled_target_time_us(target_onset_us);
    }
    s_recent_matches[s_recent_match_head] = (recent_match_t) {
        .valid = true,
        .played_onset_us = played_onset_us,
        .target_onset_us = target_onset_us,
    };
    s_recent_match_head =
        (s_recent_match_head + 1U) %
        SCREEN_RECENT_MATCH_CAPACITY;
    if (s_recent_match_count < SCREEN_RECENT_MATCH_CAPACITY) {
        ++s_recent_match_count;
    }
}

static int64_t current_score_time_ms_locked(int64_t now_us)
{
    if (!s_session_active || s_session_paused ||
        s_performance_origin_us <= 0 ||
        now_us < s_performance_origin_us) {
        return -1;
    }
    int64_t played_elapsed_us = now_us - s_performance_origin_us;
    return (int64_t)((double)played_elapsed_us *
                     (double)s_tempo_ema / 1000.0);
}

static size_t collect_expired_targets_locked(
    int64_t score_time_ms,
    settled_note_result_t *settled,
    size_t settled_capacity)
{
    if (score_time_ms < 0 || !settled || settled_capacity == 0) return 0;

    size_t settled_count = 0;
    for (size_t index = s_aligned_index;
         index < s_target_score.note_count &&
         index < SCORE_DATA_MAX_NOTES;
         ++index) {
        if (s_target_consumed[index]) continue;
        const score_note_t *target = &s_target_score.notes[index];
        uint64_t deadline_ms =
            (uint64_t)target->start_ms +
            (uint64_t)target->duration_ms +
            SCREEN_TARGET_FINAL_GRACE_MS;
        if ((uint64_t)score_time_ms < deadline_ms) continue;

        s_target_consumed[index] = true;
        settled[settled_count++] =
            settle_unplayed_target_locked(index);
        if (settled_count >= settled_capacity) break;
    }
    while (s_aligned_index < s_target_score.note_count &&
           s_aligned_index < SCORE_DATA_MAX_NOTES &&
           s_target_consumed[s_aligned_index]) {
        ++s_aligned_index;
    }
    return settled_count;
}

static void apply_settled_results(const settled_note_result_t *settled,
                                  size_t settled_count)
{
    for (size_t index = 0; index < settled_count; ++index) {
        music_display_apply_note_result(
            (int)settled[index].target_index + 1,
            settled[index].expected_note,
            settled[index].played_note, 0.0f,
            settled[index].pitch_ok, settled[index].pitch_ok);
    }
}

static void practice_follow_task(void *argument)
{
    (void)argument;
    int64_t last_page_check_ms = -1;
    while (true) {
        settled_note_result_t settled[SCREEN_MISSING_BATCH_CAPACITY];
        size_t settled_count = 0;
        int64_t score_time_ms = -1;
        bool queue_finish = false;

        if (s_session_lock != NULL) {
            xSemaphoreTake(s_session_lock, portMAX_DELAY);
            score_time_ms =
                current_score_time_ms_locked(esp_timer_get_time());
            if (score_time_ms >= 0) {
                settled_count = collect_expired_targets_locked(
                    score_time_ms, settled,
                    SCREEN_MISSING_BATCH_CAPACITY);
                if ((uint64_t)score_time_ms >=
                        (uint64_t)s_score_end_ms +
                            SCREEN_SESSION_FINISH_GRACE_MS &&
                    s_aligned_index >= s_target_score.note_count &&
                    !s_auto_finish_queued) {
                    s_auto_finish_queued = true;
                    queue_finish = true;
                }
            }
            xSemaphoreGive(s_session_lock);
        }

        apply_settled_results(settled, settled_count);
        if (score_time_ms >= 0 &&
            (last_page_check_ms < 0 ||
             score_time_ms - last_page_check_ms >=
                 SCREEN_PAGE_CHECK_PERIOD_MS)) {
            music_display_check_time_page_turn(score_time_ms);
            last_page_check_ms = score_time_ms;
        } else if (score_time_ms < 0) {
            last_page_check_ms = -1;
        }
        if (queue_finish) queue_automatic_finish();
        vTaskDelay(pdMS_TO_TICKS(SCREEN_FOLLOW_TASK_PERIOD_MS));
    }
}

static void release_target_locked(void)
{
    score_document_release(&s_target_score);
    memset(&s_target_score, 0, sizeof(s_target_score));
    reset_alignment_locked();
}

static void end_active_session(void)
{
    bool was_active = false;
    if (s_session_lock != NULL) {
        xSemaphoreTake(s_session_lock, portMAX_DELAY);
        was_active = s_session_active;
        s_session_active = false;
        s_session_paused = false;
        s_session_input = INPUT_SOURCE_NONE;
        s_pause_started_us = 0;
        ++s_audio_countdown_generation;
        release_target_locked();
        xSemaphoreGive(s_session_lock);
    }
    if (was_active) {
        notify_camera_practice_state(S3_CAMERA_PRACTICE_FINISHED);
    }
    hide_audio_countdown_overlay();
    music_display_set_practice_navigation_state(false, false);
    if (s_metronome_running) {
        speaker_service_metronome_stop();
        s_metronome_running = false;
    }
}

static void result_back_event_cb(lv_event_t *event)
{
    (void)event;
    end_active_session();
    hide_practice_buttons();
    music_display_set_creator_active(false);
    music_display_set_read_only(false);
    esp_err_t err = scoring_service_reset();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "unable to reset completed practice: %s",
                 esp_err_to_name(err));
    }
    score_ui_flow_open_choose(&guider_ui);
}

static void ensure_result_back_button(void)
{
    bsp_display_lock(portMAX_DELAY);
    if (!guider_ui.end_screen || !lv_obj_is_valid(guider_ui.end_screen)) {
        bsp_display_unlock();
        return;
    }
    if (!s_result_back_button || !lv_obj_is_valid(s_result_back_button)) {
        s_result_back_button = lv_button_create(guider_ui.end_screen);
        lv_obj_set_pos(s_result_back_button, 24, 20);
        lv_obj_set_size(s_result_back_button, 138, 48);
        lv_obj_set_style_radius(s_result_back_button, 10, 0);
        lv_obj_set_style_bg_color(s_result_back_button,
                                  lv_color_hex(0x6B3515), 0);
        lv_obj_t *label = lv_label_create(s_result_back_button);
        lv_label_set_text(label, LV_SYMBOL_LEFT " \xE8\xBF\x94\xE5\x9B\x9E\xE9\x80\x89\xE6\x9B\xB2");
        const lv_font_t *font = app_font_chinese_22();
        if (font) lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(label);
        lv_obj_add_event_cb(s_result_back_button, result_back_event_cb,
                            LV_EVENT_CLICKED, NULL);
    }
    lv_obj_clear_flag(s_result_back_button, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

typedef struct {
    char *json;
    size_t length;
} result_copy_t;

static esp_err_t copy_result_json(const char *json,
                                  size_t length,
                                  void *context)
{
    result_copy_t *copy = context;
    copy->json = malloc(length + 1U);
    if (copy->json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy->json, json, length);
    copy->json[length] = '\0';
    copy->length = length;
    return ESP_OK;
}

static void set_result_task_running(bool running)
{
    if (s_session_lock == NULL) {
        s_result_task_running = running;
        return;
    }
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    s_result_task_running = running;
    xSemaphoreGive(s_session_lock);
}

static void mark_preparation_ready_after_result(void)
{
    if (s_session_lock == NULL) return;
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if (s_preparation.valid) {
        s_preparation.phase = SCREEN_PREPARATION_PREPARED;
        s_preparation.last_error = ESP_OK;
        snprintf(s_preparation.message, sizeof(s_preparation.message),
                 "评分完成，可重新练习");
        ++s_preparation.revision;
    }
    xSemaphoreGive(s_session_lock);
}

static void result_task(void *argument)
{
    (void)argument;
    esp_err_t err = scoring_service_wait_for_result(
        SCREEN_RESULT_TIMEOUT_MS);
    result_copy_t result = {0};
    result_action_t action = s_result_action;
    if (err == ESP_OK && action == RESULT_ACTION_SHOW) {
        err = scoring_service_with_result(copy_result_json, &result);
    }

    /* A later voice command may replace "show score" with home/restart while
     * scoring is still running. Read the action again before changing pages. */
    action = s_result_action;

    if (action == RESULT_ACTION_SHOW) {
        if (err == ESP_OK && result.json != NULL) {
            music_display_show_score(result.json);
            ensure_result_back_button();
            mark_preparation_ready_after_result();
        } else {
            ESP_LOGE(TAG, "score result unavailable: %s",
                     esp_err_to_name(err));
            set_music_status("\xE8\xAF\x84\xE5\x88\x86\xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x8C\xE8\xAF\xB7\xE8\xBF\x94\xE5\x9B\x9E\xE9\x87\x8D\xE8\xAF\x95");
        }
        free(result.json);
        set_result_task_running(false);
        vTaskDelete(NULL);
        return;
    }

    free(result.json);
    esp_err_t reset_error = scoring_service_reset();
    if (reset_error != ESP_OK) {
        ESP_LOGE(TAG, "unable to reset discarded result: %s",
                 esp_err_to_name(reset_error));
        if (action == RESULT_ACTION_RESTART) {
            set_music_status("\xE6\x97\xA0\xE6\xB3\x95\xE9\x87\x8D\xE6\x96\xB0\xE5\xBC\x80\xE5\xA7\x8B");
        }
        set_result_task_running(false);
        vTaskDelete(NULL);
        return;
    }

    const bool restart = s_result_action == RESULT_ACTION_RESTART;
    set_result_task_running(false);
    if (restart) {
        esp_err_t start_error = screen_adapter_start_prepared_score();
        if (start_error != ESP_OK) {
            ESP_LOGE(TAG, "voice restart failed: %s",
                     esp_err_to_name(start_error));
            set_music_status("\xE6\x97\xA0\xE6\xB3\x95\xE9\x87\x8D\xE6\x96\xB0\xE5\xBC\x80\xE5\xA7\x8B");
        }
    }
    vTaskDelete(NULL);
}

static void stop_practice_event_cb(lv_event_t *event)
{
    (void)event;
    finish_practice();
}

static bool begin_practice_completion_internal(result_action_t action,
                                                bool allow_detached_recording)
{
    bool detached_recording = false;
    if (allow_detached_recording) {
        scoring_service_status_t status = {0};
        scoring_service_get_status(&status);
        detached_recording = status.state == SCORING_SERVICE_RECORDING ||
                             status.state == SCORING_SERVICE_PAUSED;
    }
    if (s_session_lock == NULL) return false;
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if (s_result_task_running) {
        if (action != RESULT_ACTION_SHOW) s_result_action = action;
        xSemaphoreGive(s_session_lock);
        return true;
    }
    const bool active = s_session_active || detached_recording;
    if (active) {
        /* Claim completion before releasing the lock. A phone request and a
         * screen tap can now race without stopping the recorder twice. */
        s_result_task_running = true;
        s_result_action = action;
    }
    xSemaphoreGive(s_session_lock);
    if (!active) return false;

    if (bsp_display_lock(portMAX_DELAY)) {
        if (s_stop_button && lv_obj_is_valid(s_stop_button)) {
            lv_obj_add_state(s_stop_button, LV_STATE_DISABLED);
        }
        bsp_display_unlock();
    }
    if (action == RESULT_ACTION_SHOW) {
        set_music_status("\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xAF\x84\xE5\x88\x86...");
    } else if (action == RESULT_ACTION_RESTART) {
        set_music_status("\xE6\xAD\xA3\xE5\x9C\xA8\xE9\x87\x8D\xE6\x96\xB0\xE5\xBC\x80\xE5\xA7\x8B...");
    }
    end_active_session();

    esp_err_t err = scoring_service_stop();
    if (err != ESP_OK) {
        set_result_task_running(false);
        ESP_LOGE(TAG, "unable to stop scoring: %s", esp_err_to_name(err));
        set_music_status("\xE6\x97\xA0\xE6\xB3\x95\xE5\x81\x9C\xE6\xAD\xA2\xE8\xAF\x84\xE5\x88\x86");
        if (bsp_display_lock(portMAX_DELAY)) {
            if (s_stop_button && lv_obj_is_valid(s_stop_button)) {
                lv_obj_clear_state(s_stop_button, LV_STATE_DISABLED);
            }
            bsp_display_unlock();
        }
        return false;
    }

    if (xTaskCreate(result_task, "screen_result",
                    SCREEN_RESULT_TASK_STACK_BYTES, NULL,
                    SCREEN_TASK_PRIORITY, NULL) != pdPASS) {
        set_result_task_running(false);
        set_music_status("\xE8\xAF\x84\xE5\x88\x86\xE4\xBB\xBB\xE5\x8A\xA1\xE5\x88\x9B\xE5\xBB\xBA\xE5\xA4\xB1\xE8\xB4\xA5");
        return false;
    }
    return true;
}

static bool begin_practice_completion(result_action_t action)
{
    return begin_practice_completion_internal(action, false);
}

static void finish_practice(void)
{
    (void)begin_practice_completion(RESULT_ACTION_SHOW);
}

esp_err_t screen_adapter_complete_practice(void)
{
    if (begin_practice_completion_internal(RESULT_ACTION_SHOW, true)) {
        return ESP_OK;
    }

    /* Idempotent remote stop: if another caller already moved scoring past
     * recording, the HTTP request should wait for/read that same result. */
    scoring_service_status_t status = {0};
    scoring_service_get_status(&status);
    if (status.state == SCORING_SERVICE_SCORING ||
        status.state == SCORING_SERVICE_READY) {
        return ESP_OK;
    }
    return status.last_error != ESP_OK
               ? status.last_error
               : ESP_ERR_INVALID_STATE;
}

static void finish_practice_async_cb(void *user_data)
{
    (void)user_data;
    if (s_session_lock != NULL) {
        xSemaphoreTake(s_session_lock, portMAX_DELAY);
        s_auto_finish_queued = false;
        xSemaphoreGive(s_session_lock);
    }
    finish_practice();
}

static void queue_automatic_finish(void)
{
    lv_result_t result = LV_RESULT_INVALID;
    if (bsp_display_lock(portMAX_DELAY)) {
        result = lv_async_call(finish_practice_async_cb, NULL);
        bsp_display_unlock();
    }
    if (result == LV_RESULT_OK) return;

    ESP_LOGE(TAG, "unable to queue automatic practice finish");
    if (s_session_lock != NULL) {
        xSemaphoreTake(s_session_lock, portMAX_DELAY);
        s_auto_finish_queued = false;
        xSemaphoreGive(s_session_lock);
    }
}

static void ensure_stop_button(void)
{
    bsp_display_lock(portMAX_DELAY);
    if (!guider_ui.music_screen || !lv_obj_is_valid(guider_ui.music_screen)) {
        bsp_display_unlock();
        return;
    }
    if (!s_stop_button || !lv_obj_is_valid(s_stop_button)) {
        s_stop_button = lv_button_create(guider_ui.music_screen);
        lv_obj_set_pos(s_stop_button, 162, 540);
        lv_obj_set_size(s_stop_button, 142, 48);
        lv_obj_set_style_radius(s_stop_button, 10, 0);
        lv_obj_set_style_bg_color(s_stop_button,
                                  lv_color_hex(0x8A3B22), 0);
        lv_obj_t *label = lv_label_create(s_stop_button);
        lv_label_set_text(label, "\xE7\xBB\x93\xE6\x9D\x9F\xE6\xBC\x94\xE5\xA5\x8F");
        const lv_font_t *font = app_font_chinese_22();
        if (font) lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(label);
        lv_obj_add_event_cb(s_stop_button, stop_practice_event_cb,
                            LV_EVENT_CLICKED, NULL);
    }
    lv_obj_clear_state(s_stop_button, LV_STATE_DISABLED);
    lv_obj_clear_flag(s_stop_button, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

static void set_pause_button_state(bool paused)
{
    if (!s_pause_label || !lv_obj_is_valid(s_pause_label)) return;
    lv_label_set_text(s_pause_label,
                      paused ? "\xE7\xBB\xA7\xE7\xBB\xAD"
                              : "\xE6\x9A\x82\xE5\x81\x9C");
}

static bool pause_active_practice(void)
{
    if (s_session_lock == NULL) return false;

    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    bool active = s_session_active;
    bool paused = s_session_paused;
    xSemaphoreGive(s_session_lock);
    if (!active || paused) return false;

    esp_err_t err = scoring_service_pause();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "unable to pause practice: %s",
                 esp_err_to_name(err));
        return false;
    }

    int64_t now_us = esp_timer_get_time();
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    s_session_paused = true;
    s_pause_started_us = now_us;
    xSemaphoreGive(s_session_lock);
    music_display_set_practice_navigation_state(true, true);
    set_pause_button_state(true);
    set_music_status("\xE5\xB7\xB2\xE6\x9A\x82\xE5\x81\x9C");
    notify_camera_practice_state(S3_CAMERA_PRACTICE_PAUSED);
    return true;
}

static void pause_practice_event_cb(lv_event_t *event)
{
    (void)event;
    if (s_session_lock == NULL) return;

    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    bool active = s_session_active;
    bool paused = s_session_paused;
    xSemaphoreGive(s_session_lock);
    if (!active) return;

    int64_t now_us = esp_timer_get_time();
    if (!paused) {
        (void)pause_active_practice();
        return;
    }

    esp_err_t err = scoring_service_resume();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "unable to resume practice: %s",
                 esp_err_to_name(err));
        return;
    }
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if (s_pause_started_us > 0 && now_us > s_pause_started_us) {
        int64_t pause_duration_us = now_us - s_pause_started_us;
        if (s_performance_origin_us != 0) {
            s_performance_origin_us += pause_duration_us;
        }
        for (size_t index = 0;
             index < SCREEN_PENDING_NOTE_CAPACITY; ++index) {
            if (s_pending_notes[index].active) {
                s_pending_notes[index].played_onset_us +=
                    pause_duration_us;
            }
        }
        for (size_t index = 0;
             index < SCREEN_RECENT_MATCH_CAPACITY; ++index) {
            if (s_recent_matches[index].valid) {
                s_recent_matches[index].played_onset_us +=
                    pause_duration_us;
            }
        }
    }
    s_pause_started_us = 0;
    s_session_paused = false;
    xSemaphoreGive(s_session_lock);
    music_display_set_practice_navigation_state(true, false);
    set_pause_button_state(false);
    set_music_status("\xE6\xBC\x94\xE5\xA5\x8F\xE4\xB8\xAD..");
    notify_camera_practice_state(S3_CAMERA_PRACTICE_PLAYING);
}

static void ensure_pause_button(void)
{
    bsp_display_lock(portMAX_DELAY);
    if (!guider_ui.music_screen || !lv_obj_is_valid(guider_ui.music_screen)) {
        bsp_display_unlock();
        return;
    }
    if (!s_pause_button || !lv_obj_is_valid(s_pause_button)) {
        s_pause_button = lv_button_create(guider_ui.music_screen);
        lv_obj_set_pos(s_pause_button, 12, 540);
        lv_obj_set_size(s_pause_button, 138, 48);
        lv_obj_set_style_radius(s_pause_button, 10, 0);
        lv_obj_set_style_bg_color(s_pause_button,
                                  lv_color_hex(0x725131), 0);
        s_pause_label = lv_label_create(s_pause_button);
        const lv_font_t *font = app_font_chinese_22();
        if (font) lv_obj_set_style_text_font(s_pause_label, font, 0);
        lv_obj_set_style_text_color(s_pause_label,
                                    lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(s_pause_label);
        lv_obj_add_event_cb(s_pause_button, pause_practice_event_cb,
                            LV_EVENT_CLICKED, NULL);
    }
    set_pause_button_state(false);
    lv_obj_clear_state(s_pause_button, LV_STATE_DISABLED);
    lv_obj_clear_flag(s_pause_button, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

static void set_pause_button_enabled(bool enabled)
{
    bsp_display_lock(portMAX_DELAY);
    if (s_pause_button && lv_obj_is_valid(s_pause_button)) {
        if (enabled) {
            lv_obj_clear_state(s_pause_button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_pause_button, LV_STATE_DISABLED);
        }
    }
    bsp_display_unlock();
}

static bool audio_countdown_is_current(uint32_t generation)
{
    if (s_session_lock == NULL) return false;
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    const bool current =
        s_session_active && s_session_paused &&
        s_session_input == INPUT_SOURCE_AUDIO_S3 &&
        s_audio_countdown_generation == generation;
    xSemaphoreGive(s_session_lock);
    return current;
}

static void audio_countdown_task(void *argument)
{
    audio_countdown_context_t *context = argument;
    for (int seconds = SCREEN_AUDIO_COUNTDOWN_SECONDS;
         seconds > 0; --seconds) {
        if (!audio_countdown_is_current(context->generation)) break;
        show_audio_countdown_overlay(seconds);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    bool resumed = false;
    esp_err_t resume_error = ESP_ERR_INVALID_STATE;
    if (s_session_lock != NULL) {
        xSemaphoreTake(s_session_lock, portMAX_DELAY);
        if (s_session_active && s_session_paused &&
            s_session_input == INPUT_SOURCE_AUDIO_S3 &&
            s_audio_countdown_generation == context->generation) {
            resume_error = scoring_service_resume();
            if (resume_error == ESP_OK) {
                const int64_t now_us = esp_timer_get_time();
                /* Countdown completion is score time zero. Preserve any
                 * intentional opening rest in the score instead of jumping
                 * directly to its first note. */
                s_performance_origin_us = now_us;
                if (s_performance_origin_us <= 0) {
                    s_performance_origin_us = 1;
                }
                s_session_paused = false;
                s_pause_started_us = 0;
                s_first_match_pending = true;
                resumed = true;
            }
        }
        xSemaphoreGive(s_session_lock);
    }

    hide_audio_countdown_overlay();
    if (resumed) {
        music_display_set_practice_navigation_state(true, false);
        set_pause_button_state(false);
        set_pause_button_enabled(true);
        set_music_status("\xE6\xBC\x94\xE5\xA5\x8F\xE4\xB8\xAD..");
        notify_camera_practice_state(S3_CAMERA_PRACTICE_PLAYING);

        if (context->metronome_enabled) {
            esp_err_t err = speaker_service_metronome_start(
                context->metronome_bpm, context->time_sig_num,
                context->time_sig_den);
            if (err == ESP_OK) {
                s_metronome_running = true;
            } else {
                ESP_LOGW(TAG, "metronome unavailable after countdown: %s",
                         esp_err_to_name(err));
            }
        }
        ESP_LOGI(TAG, "audio_s3 countdown complete; following started");
    } else if (audio_countdown_is_current(context->generation)) {
        ESP_LOGE(TAG, "unable to resume audio_s3 after countdown: %s",
                 esp_err_to_name(resume_error));
        set_music_status("\xE9\xBA\xA6\xE5\x85\x8B\xE9\xA3\x8E\xE5\x90\xAF\xE5\x8A\xA8\xE5\xA4\xB1\xE8\xB4\xA5");
    }

    free(context);
    vTaskDelete(NULL);
}

static bool start_audio_countdown(uint32_t generation,
                                  const score_practice_options_t *options)
{
    audio_countdown_context_t *context = calloc(1, sizeof(*context));
    if (!context) return false;
    context->generation = generation;
    context->metronome_enabled = options->metronome_enabled;
    context->metronome_bpm = (uint16_t)options->metronome_bpm;
    context->time_sig_num = (uint8_t)options->metronome_time_sig_num;
    context->time_sig_den = (uint8_t)options->metronome_time_sig_den;
    if (xTaskCreate(audio_countdown_task, "audio_countdown",
                    SCREEN_AUDIO_COUNTDOWN_TASK_STACK_BYTES, context,
                    SCREEN_TASK_PRIORITY, NULL) != pdPASS) {
        free(context);
        return false;
    }
    return true;
}

static void hide_practice_buttons(void)
{
    bsp_display_lock(portMAX_DELAY);
    if (s_stop_button && lv_obj_is_valid(s_stop_button))
        lv_obj_add_flag(s_stop_button, LV_OBJ_FLAG_HIDDEN);
    if (s_pause_button && lv_obj_is_valid(s_pause_button))
        lv_obj_add_flag(s_pause_button, LV_OBJ_FLAG_HIDDEN);
    bsp_display_unlock();
}

static void set_preparation_error(esp_err_t error, const char *message)
{
    if (s_session_lock == NULL) return;
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    s_preparation.phase = SCREEN_PREPARATION_ERROR;
    s_preparation.last_error = error;
    snprintf(s_preparation.message, sizeof(s_preparation.message), "%s",
             message ? message : esp_err_to_name(error));
    ++s_preparation.revision;
    xSemaphoreGive(s_session_lock);
}

static bool preparation_metadata_from_json(const char *json,
                                           size_t json_length,
                                           const char *source_name,
                                           score_info_t *score)
{
    if (!json || json_length == 0 || !source_name || !score) return false;
    cJSON *root = cJSON_ParseWithLength(json, json_length);
    if (!root) return false;

    memset(score, 0, sizeof(*score));
    snprintf(score->filename, sizeof(score->filename), "%s", source_name);
    cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "title");
    cJSON *bpm = cJSON_GetObjectItemCaseSensitive(root, "bpm");
    cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "key");
    cJSON *time_signature =
        cJSON_GetObjectItemCaseSensitive(root, "time_signature");
    cJSON *notes = cJSON_GetObjectItemCaseSensitive(root, "notes");
    snprintf(score->title, sizeof(score->title), "%s",
             cJSON_IsString(title) && title->valuestring[0]
                 ? title->valuestring : source_name);
    snprintf(score->key, sizeof(score->key), "%s",
             cJSON_IsString(key) && key->valuestring[0]
                 ? key->valuestring : "C major");
    snprintf(score->time_signature, sizeof(score->time_signature), "%s",
             cJSON_IsString(time_signature) &&
             time_signature->valuestring[0]
                 ? time_signature->valuestring : "4/4");
    score->bpm = cJSON_IsNumber(bpm) && bpm->valueint > 0
                     ? bpm->valueint : 120;
    score->note_count = cJSON_IsArray(notes)
                            ? cJSON_GetArraySize(notes) : 0;
    cJSON_Delete(root);
    return score->note_count > 0;
}

static bool preparation_changed_from_screen(
    const score_info_t *score,
    const score_practice_options_t *options,
    void *user_data)
{
    (void)user_data;
    if (!score || !options || s_session_lock == NULL) return false;

    bool needs_json = false;
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    needs_json = s_preparation_json == NULL ||
                 strcmp(s_preparation.filename, score->filename) != 0;
    xSemaphoreGive(s_session_lock);

    char *loaded_json = NULL;
    size_t loaded_length = 0;
    if (needs_json) {
        if (!score_storage_load(score->filename) ||
            !score_storage_read_json(score->filename, &loaded_json,
                                     &loaded_length)) {
            free(loaded_json);
            set_preparation_error(ESP_ERR_NOT_FOUND,
                                  "无法读取所选乐谱");
            return false;
        }
    }

    int time_num = 4;
    int time_den = 4;
    sscanf(score->time_signature, "%d/%d", &time_num, &time_den);
    if (time_num <= 0 || time_den <= 0) {
        time_num = 4;
        time_den = 4;
    }

    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if (loaded_json) {
        free(s_preparation_json);
        s_preparation_json = loaded_json;
        s_preparation_json_length = loaded_length;
    }
    s_preparation.valid = true;
    s_preparation.phase = SCREEN_PREPARATION_PREPARED;
    s_preparation.notation =
        options->notation_type == SCORE_NOTATION_NUMBERED
            ? SCREEN_PREPARATION_NUMBERED
            : SCREEN_PREPARATION_STAFF;
    s_preparation.mode = options->read_only
                             ? SCREEN_PREPARATION_READ_ONLY
                             : SCREEN_PREPARATION_FOLLOW;
    s_preparation.input =
        options->input_source == SCORE_INPUT_USB_MIDI
            ? SCREEN_PREPARATION_USB_MIDI
            : SCREEN_PREPARATION_AUDIO_S3;
    snprintf(s_preparation.filename, sizeof(s_preparation.filename), "%s",
             score->filename);
    snprintf(s_preparation.title, sizeof(s_preparation.title), "%s",
             score->title);
    snprintf(s_preparation.key, sizeof(s_preparation.key), "%s",
             score->key[0] ? score->key : "C major");
    s_preparation.bpm = options->score_bpm;
    s_preparation.time_sig_num = time_num;
    s_preparation.time_sig_den = time_den;
    s_preparation.note_count = score->note_count;
    s_preparation.last_error = ESP_OK;
    snprintf(s_preparation.message, sizeof(s_preparation.message),
             "乐谱准备就绪");
    ++s_preparation.revision;
    xSemaphoreGive(s_session_lock);
    return true;
}

static input_source_t selected_input_source(score_input_source_t source)
{
    return source == SCORE_INPUT_USB_MIDI ? INPUT_SOURCE_USB_MIDI
                                         : INPUT_SOURCE_AUDIO_S3;
}

static void log_practice_start_task_diagnostics(void)
{
    const void *stack_pointer = esp_cpu_get_sp();
    ESP_LOGI(TAG,
             "practice start task: name=%s stack_sp=%p external=%s "
             "high_water=%u internal_free=%u psram_free=%u",
             pcTaskGetName(NULL), stack_pointer,
             esp_ptr_external_ram(stack_pointer) ? "yes" : "no",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM |
                                                MALLOC_CAP_8BIT));
}

static bool start_selected_score(const char *filename,
                                 const score_practice_options_t *options,
                                 void *user_data)
{
    (void)user_data;
    if (filename == NULL || options == NULL) return false;
    log_practice_start_task_diagnostics();
    if (creator_mode_is_active()) {
        ESP_LOGW(TAG, "practice rejected while Creator Mode is active");
        set_preparation_error(ESP_ERR_INVALID_STATE,
                              "创作者模式运行中，无法开始");
        return false;
    }
    if (!options->read_only &&
        options->input_source == SCORE_INPUT_USB_MIDI) {
        input_source_status_t input_status = {0};
        input_source_manager_get_status(&input_status);
        if (!input_status.usb_midi_connected) {
            score_ui_flow_set_prepare_status(
                "未检测到MIDI设备");
            set_preparation_error(ESP_ERR_INVALID_STATE,
                                  "未检测到MIDI设备");
            ESP_LOGW(TAG, "practice start rejected: no USB MIDI device");
            return false;
        }
    }

    char *json = NULL;
    size_t json_length = 0;
    if (s_session_lock != NULL) {
        xSemaphoreTake(s_session_lock, portMAX_DELAY);
        bool prepared = s_preparation.valid &&
                        s_preparation_json != NULL &&
                        strcmp(s_preparation.filename, filename) == 0;
        if (prepared) {
            json = malloc(s_preparation_json_length + 1U);
            if (json != NULL) {
                memcpy(json, s_preparation_json,
                       s_preparation_json_length + 1U);
                json_length = s_preparation_json_length;
            }
            s_preparation.phase = SCREEN_PREPARATION_STARTING;
            s_preparation.notation =
                options->notation_type == SCORE_NOTATION_NUMBERED
                    ? SCREEN_PREPARATION_NUMBERED
                    : SCREEN_PREPARATION_STAFF;
            s_preparation.mode = options->read_only
                                     ? SCREEN_PREPARATION_READ_ONLY
                                     : SCREEN_PREPARATION_FOLLOW;
            s_preparation.input =
                options->input_source == SCORE_INPUT_USB_MIDI
                    ? SCREEN_PREPARATION_USB_MIDI
                    : SCREEN_PREPARATION_AUDIO_S3;
            s_preparation.last_error = ESP_OK;
            snprintf(s_preparation.message, sizeof(s_preparation.message),
                     "正在载入乐谱");
            ++s_preparation.revision;
        }
        xSemaphoreGive(s_session_lock);
    }
    if (json == NULL &&
        !score_storage_read_json(filename, &json, &json_length)) {
        ESP_LOGE(TAG, "unable to read score: %s", filename);
        set_preparation_error(ESP_ERR_NOT_FOUND, "无法读取所选乐谱");
        return false;
    }
    if (json == NULL) {
        set_preparation_error(ESP_ERR_NO_MEM, "乐谱内存不足");
        return false;
    }

    music_display_set_creator_active(false);
    music_display_set_read_only(options->read_only);
    int practice_bpm = options->metronome_enabled
                           ? options->metronome_bpm
                           : options->score_bpm;
    music_display_score_options_t display_options = {
        .tempo_bpm = practice_bpm,
        .time_sig_num = options->score_time_sig_num,
        .time_sig_den = options->score_time_sig_den,
        .notation_type = options->notation_type == SCORE_NOTATION_NUMBERED
                             ? MUSIC_DISPLAY_NOTATION_NUMBERED
                             : MUSIC_DISPLAY_NOTATION_STAFF,
    };

    if (options->read_only) {
        music_display_set_practice_navigation_state(false, false);
        hide_practice_buttons();
        bool displayed = music_display_apply_score_json_with_options(
            json, &display_options);
        free(json);
        if (displayed) {
            xSemaphoreTake(s_session_lock, portMAX_DELAY);
            s_preparation.phase = SCREEN_PREPARATION_READING;
            s_preparation.last_error = ESP_OK;
            snprintf(s_preparation.message, sizeof(s_preparation.message),
                     "正在只读看谱");
            ++s_preparation.revision;
            xSemaphoreGive(s_session_lock);
        } else {
            set_preparation_error(ESP_FAIL, "乐谱显示失败");
        }
        return displayed;
    }

    char parse_error[96] = {0};
    size_t note_count = 0;
    esp_err_t err = screen_score_bridge_parse_and_store(
        json, json_length, practice_bpm, parse_error, sizeof(parse_error),
        &note_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "score parse failed: %s (%s)",
                 parse_error, esp_err_to_name(err));
        free(json);
        set_preparation_error(err, "乐谱解析失败");
        return false;
    }

    input_source_t input = selected_input_source(options->input_source);
    err = input_source_manager_select(input);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "input selection failed: %s", esp_err_to_name(err));
        free(json);
        set_preparation_error(err, "输入源不可用");
        return false;
    }

    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    release_target_locked();
    err = score_data_copy(&s_target_score);
    xSemaphoreGive(s_session_lock);
    if (err != ESP_OK) {
        free(json);
        set_preparation_error(err, "无法复制评分乐谱");
        return false;
    }

    err = scoring_service_start(input_source_name(input),
                                "beginner_mono_v2");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scoring start failed for %s: %s",
                 input_source_name(input), esp_err_to_name(err));
        xSemaphoreTake(s_session_lock, portMAX_DELAY);
        release_target_locked();
        xSemaphoreGive(s_session_lock);
        free(json);
        set_preparation_error(err,
                              input == INPUT_SOURCE_USB_MIDI
                                  ? "USB MIDI 未连接或评分启动失败"
                                  : "音频输入未连接或评分启动失败");
        return false;
    }

    const bool audio_countdown = input == INPUT_SOURCE_AUDIO_S3;
    if (audio_countdown) {
        err = scoring_service_pause();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "unable to arm audio_s3 countdown: %s",
                     esp_err_to_name(err));
            scoring_service_stop();
            xSemaphoreTake(s_session_lock, portMAX_DELAY);
            release_target_locked();
            xSemaphoreGive(s_session_lock);
            free(json);
            set_preparation_error(err,
                                  "\xE9\xBA\xA6\xE5\x85\x8B\xE9\xA3\x8E\xE5\x80\x92\xE8\xAE\xA1\xE6\x97\xB6\xE5\x90\xAF\xE5\x8A\xA8\xE5\xA4\xB1\xE8\xB4\xA5");
            return false;
        }
    }

    bool displayed = music_display_apply_score_json_with_options(
        json, &display_options);
    free(json);
    if (!displayed) {
        scoring_service_stop();
        end_active_session();
        set_preparation_error(ESP_FAIL, "乐谱显示失败");
        return false;
    }

    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    s_session_active = true;
    s_session_paused = audio_countdown;
    s_session_input = input;
    s_pause_started_us = audio_countdown ? esp_timer_get_time() : 0;
    uint32_t countdown_generation = s_audio_countdown_generation;
    if (audio_countdown) {
        countdown_generation = ++s_audio_countdown_generation;
    }
    reset_alignment_locked();
    s_first_match_pending = audio_countdown;
    for (size_t index = 0; index < s_target_score.note_count; ++index) {
        uint64_t note_end_ms =
            (uint64_t)s_target_score.notes[index].start_ms +
            (uint64_t)s_target_score.notes[index].duration_ms;
        if (note_end_ms > UINT32_MAX) note_end_ms = UINT32_MAX;
        if ((uint32_t)note_end_ms > s_score_end_ms) {
            s_score_end_ms = (uint32_t)note_end_ms;
        }
    }
    xSemaphoreGive(s_session_lock);
    music_display_set_practice_navigation_state(true, audio_countdown);
    notify_camera_practice_state(
        audio_countdown ? S3_CAMERA_PRACTICE_PAUSED
                        : S3_CAMERA_PRACTICE_PLAYING);

    if (!audio_countdown && options->metronome_enabled) {
        err = speaker_service_metronome_start(
            (uint16_t)options->metronome_bpm,
            (uint8_t)options->metronome_time_sig_num,
            (uint8_t)options->metronome_time_sig_den);
        if (err == ESP_OK) {
            s_metronome_running = true;
        } else {
            ESP_LOGW(TAG, "metronome unavailable: %s",
                     esp_err_to_name(err));
        }
    }
    ensure_stop_button();
    ensure_pause_button();
    if (audio_countdown) {
        set_pause_button_enabled(false);
        set_music_status("\xE9\xBA\xA6\xE5\x85\x8B\xE9\xA3\x8E\xE5\x87\x86\xE5\xA4\x87\xE4\xB8\xAD");
        if (!start_audio_countdown(countdown_generation, options)) {
            ESP_LOGE(TAG, "unable to create audio_s3 countdown task");
            err = scoring_service_resume();
            bool resumed_after_fallback = err == ESP_OK;
            xSemaphoreTake(s_session_lock, portMAX_DELAY);
            if (err == ESP_OK && s_session_active &&
                s_audio_countdown_generation == countdown_generation) {
                const int64_t now_us = esp_timer_get_time();
                s_performance_origin_us = now_us;
                if (s_performance_origin_us <= 0) {
                    s_performance_origin_us = 1;
                }
                s_session_paused = false;
                s_pause_started_us = 0;
                s_first_match_pending = true;
            }
            xSemaphoreGive(s_session_lock);
            music_display_set_practice_navigation_state(true, false);
            set_pause_button_enabled(true);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "unable to resume audio_s3 after countdown task failure: %s",
                         esp_err_to_name(err));
            } else if (options->metronome_enabled) {
                err = speaker_service_metronome_start(
                    (uint16_t)options->metronome_bpm,
                    (uint8_t)options->metronome_time_sig_num,
                    (uint8_t)options->metronome_time_sig_den);
                if (err == ESP_OK) s_metronome_running = true;
            }
            if (resumed_after_fallback) {
                notify_camera_practice_state(
                    S3_CAMERA_PRACTICE_PLAYING);
            }
        }
    }
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    s_preparation.phase = SCREEN_PREPARATION_FOLLOWING;
    s_preparation.last_error = ESP_OK;
    snprintf(s_preparation.message, sizeof(s_preparation.message),
             "正在演奏跟谱");
    ++s_preparation.revision;
    xSemaphoreGive(s_session_lock);
    ESP_LOGI(TAG, "screen practice started: %s notes=%u bpm=%d",
             filename, (unsigned)note_count, practice_bpm);
    return true;
}

static void volume_event_cb(lv_event_t *event)
{
    if (s_applying_volume_status) return;
    int32_t value = lv_slider_get_value(lv_event_get_target(event));
    if (value < 0) value = 0;
    if (value > 80) value = 80;
    esp_err_t err = speaker_service_set_volume((uint8_t)value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "volume update failed: %s", esp_err_to_name(err));
    }
}

static void ui_watch_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    lv_obj_t *slider = guider_ui.setting_screen_voice_slide;
    if (slider && lv_obj_is_valid(slider) && slider != s_watched_volume_slider) {
        s_watched_volume_slider = slider;
        lv_slider_set_range(slider, 0, 80);
        lv_obj_add_event_cb(slider, volume_event_cb,
                            LV_EVENT_VALUE_CHANGED, NULL);
    }
    if (slider && lv_obj_is_valid(slider) &&
        !lv_obj_has_state(slider, LV_STATE_PRESSED)) {
        speaker_status_t status = {0};
        speaker_service_get_status(&status);
        int32_t canonical = status.hardware.volume_percent;
        if (canonical < 0) canonical = 0;
        if (canonical > 80) canonical = 80;
        if (lv_slider_get_value(slider) != canonical) {
            s_applying_volume_status = true;
            lv_slider_set_value(slider, canonical, LV_ANIM_OFF);
            s_applying_volume_status = false;
        }
    }
}

static void apply_main_menu_fonts(void)
{
    if (!guider_ui.screen_choose_button_label) return;

    /* "其他" is absent from the generated primary subset. Keep the whole
     * label in the gudianChinese family by falling back only to the matching
     * 34 px supplemental subset, never to the SD Source Han font. */
    s_other_mode_font = lv_font_gudianChinese_34;
    s_other_mode_font.fallback = &lv_font_gudianChinese_34_extra;
    lv_obj_set_style_text_font(guider_ui.screen_choose_button_label,
                               &s_other_mode_font, LV_PART_MAIN);
}

static void screen_init_task(void *argument)
{
    (void)argument;
    lv_display_t *display = bsp_display_start();
    if (display == NULL) {
        ESP_LOGE(TAG, "display initialization failed; other services continue");
        vTaskDelete(NULL);
        return;
    }
    esp_err_t err = bsp_display_brightness_set(100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "backlight setup failed: %s", esp_err_to_name(err));
    }

    score_storage_ensure_mounted();
    bsp_display_lock(portMAX_DELAY);
    err = app_font_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD Chinese font unavailable; using embedded fallback");
    }
    setup_ui(&guider_ui);
    apply_main_menu_fonts();
    events_init(&guider_ui);
    score_ui_flow_set_start_callback(start_selected_score, NULL);
    score_ui_flow_set_prepare_changed_callback(
        preparation_changed_from_screen, NULL);
    err = creator_mode_init_detached(&guider_ui);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Creator Mode initialization failed: %s",
                 esp_err_to_name(err));
    }
    err = screen_modes_init(&guider_ui);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "screen mode hub initialization failed: %s",
                 esp_err_to_name(err));
    }
    s_ui_watch_timer = lv_timer_create(ui_watch_timer_cb, 250, NULL);
    bsp_display_unlock();

    music_display_start();
    if (xTaskCreate(practice_follow_task, "practice_follow",
                    SCREEN_FOLLOW_TASK_STACK_BYTES, NULL,
                    SCREEN_FOLLOW_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "unable to start real-time score follower task");
    }
    s_ready = true;
    ESP_LOGI(TAG, "complete SmartMusic screen flow ready at %dx%d",
             BSP_LCD_H_RES, BSP_LCD_V_RES);
    vTaskDelete(NULL);
}

esp_err_t screen_adapter_start(void)
{
    if (s_start_requested) return ESP_OK;
    s_session_lock = xSemaphoreCreateMutex();
    if (s_session_lock == NULL) return ESP_ERR_NO_MEM;

    s_start_requested = true;
    if (xTaskCreate(screen_init_task, "screen_init",
                    SCREEN_INIT_TASK_STACK_BYTES, NULL,
                    SCREEN_TASK_PRIORITY, NULL) != pdPASS) {
        s_start_requested = false;
        vSemaphoreDelete(s_session_lock);
        s_session_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool screen_adapter_is_ready(void)
{
    return s_ready;
}

esp_err_t screen_adapter_prepare_score_json(const char *json,
                                            size_t json_length,
                                            const char *source_name)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!json || json_length == 0 || !source_name || !source_name[0])
        return ESP_ERR_INVALID_ARG;

    screen_creator_status_t creator = {0};
    screen_adapter_creator_get_status(&creator);
    if (creator.active) return ESP_ERR_INVALID_STATE;

    scoring_service_status_t scoring = {0};
    scoring_service_get_status(&scoring);
    if (scoring.state == SCORING_SERVICE_RECORDING ||
        scoring.state == SCORING_SERVICE_PAUSED ||
        scoring.state == SCORING_SERVICE_SCORING)
        return ESP_ERR_INVALID_STATE;

    score_info_t score = {0};
    if (!preparation_metadata_from_json(json, json_length, source_name,
                                        &score))
        return ESP_ERR_INVALID_ARG;

    char *json_copy = malloc(json_length + 1U);
    if (!json_copy) return ESP_ERR_NO_MEM;
    memcpy(json_copy, json, json_length);
    json_copy[json_length] = '\0';

    int time_num = 4;
    int time_den = 4;
    sscanf(score.time_signature, "%d/%d", &time_num, &time_den);
    if (time_num <= 0 || time_den <= 0) {
        time_num = 4;
        time_den = 4;
    }

    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    free(s_preparation_json);
    s_preparation_json = json_copy;
    s_preparation_json_length = json_length;
    memset(&s_preparation, 0, sizeof(s_preparation));
    s_preparation.valid = true;
    s_preparation.screen_ready = true;
    s_preparation.phase = SCREEN_PREPARATION_PREPARED;
    s_preparation.notation = SCREEN_PREPARATION_STAFF;
    s_preparation.mode = SCREEN_PREPARATION_FOLLOW;
    s_preparation.input = SCREEN_PREPARATION_USB_MIDI;
    snprintf(s_preparation.filename, sizeof(s_preparation.filename), "%s",
             score.filename);
    snprintf(s_preparation.title, sizeof(s_preparation.title), "%s",
             score.title);
    snprintf(s_preparation.key, sizeof(s_preparation.key), "%s", score.key);
    s_preparation.bpm = score.bpm;
    s_preparation.time_sig_num = time_num;
    s_preparation.time_sig_den = time_den;
    s_preparation.note_count = score.note_count;
    s_preparation.last_error = ESP_OK;
    snprintf(s_preparation.message, sizeof(s_preparation.message),
             "乐谱准备就绪");
    s_preparation.revision = 1;
    xSemaphoreGive(s_session_lock);

    bsp_display_lock(portMAX_DELAY);
    bool opened = score_ui_flow_open_preparation(
        &guider_ui, &score, false);
    bsp_display_unlock();
    if (!opened) {
        set_preparation_error(ESP_FAIL, "无法打开乐谱准备页");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t screen_adapter_prepare_sd_score(const char *filename)
{
    if (!filename || !score_storage_is_valid_sd_filename(filename))
        return ESP_ERR_INVALID_ARG;
    if (!score_storage_load_sd(filename)) return ESP_ERR_INVALID_ARG;
    char *json = NULL;
    size_t json_length = 0;
    if (!score_storage_read_sd_json(filename, &json, &json_length))
        return ESP_ERR_NOT_FOUND;
    esp_err_t err = screen_adapter_prepare_score_json(
        json, json_length, filename);
    free(json);
    return err;
}

esp_err_t screen_adapter_update_preparation(
    screen_preparation_notation_t notation,
    screen_preparation_mode_t mode,
    screen_preparation_input_t input)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if ((notation != SCREEN_PREPARATION_NUMBERED &&
         notation != SCREEN_PREPARATION_STAFF) ||
        (mode != SCREEN_PREPARATION_READ_ONLY &&
         mode != SCREEN_PREPARATION_FOLLOW) ||
        (input != SCREEN_PREPARATION_AUDIO_S3 &&
         input != SCREEN_PREPARATION_USB_MIDI))
        return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    bool can_update = s_preparation.valid &&
                      (s_preparation.phase == SCREEN_PREPARATION_PREPARED ||
                       s_preparation.phase == SCREEN_PREPARATION_ERROR);
    xSemaphoreGive(s_session_lock);
    if (!can_update) return ESP_ERR_INVALID_STATE;

    bsp_display_lock(portMAX_DELAY);
    bool applied = score_ui_flow_apply_preparation_options(
        input == SCREEN_PREPARATION_USB_MIDI
            ? SCORE_INPUT_USB_MIDI : SCORE_INPUT_MICROPHONE,
        notation == SCREEN_PREPARATION_NUMBERED
            ? SCORE_NOTATION_NUMBERED : SCORE_NOTATION_STAFF,
        mode == SCREEN_PREPARATION_READ_ONLY);
    bsp_display_unlock();
    return applied ? ESP_OK : ESP_FAIL;
}

esp_err_t screen_adapter_start_prepared_score(void)
{
    if (!s_ready || s_session_lock == NULL) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if (s_practice_starting || s_session_active) {
        xSemaphoreGive(s_session_lock);
        return ESP_OK;
    }
    if (!s_preparation.valid || s_result_task_running) {
        xSemaphoreGive(s_session_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_practice_starting = true;
    if (s_preparation.phase == SCREEN_PREPARATION_FOLLOWING ||
        s_preparation.phase == SCREEN_PREPARATION_READING ||
        s_preparation.phase == SCREEN_PREPARATION_ERROR) {
        s_preparation.phase = SCREEN_PREPARATION_PREPARED;
        s_preparation.last_error = ESP_OK;
        snprintf(s_preparation.message, sizeof(s_preparation.message),
                 "乐谱准备就绪");
        ++s_preparation.revision;
    }
    xSemaphoreGive(s_session_lock);

    scoring_service_status_t scoring = {0};
    scoring_service_get_status(&scoring);
    esp_err_t err = ESP_OK;
    if (scoring.state == SCORING_SERVICE_RECORDING ||
        scoring.state == SCORING_SERVICE_PAUSED ||
        scoring.state == SCORING_SERVICE_SCORING ||
        scoring.state == SCORING_SERVICE_UNINITIALIZED) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK) {
        xSemaphoreTake(s_session_lock, portMAX_DELAY);
        s_practice_starting = false;
        xSemaphoreGive(s_session_lock);
        return err;
    }

    score_info_t score = {0};
    score_practice_options_t options = {0};
    bsp_display_lock(portMAX_DELAY);
    bool copied = score_ui_flow_copy_preparation(&score, &options);
    if (copied) score_ui_flow_set_prepare_status("正在载入乐谱…");
    bsp_display_unlock();
    err = copied && start_selected_score(score.filename, &options, NULL)
              ? ESP_OK
              : copied ? ESP_FAIL : ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    s_practice_starting = false;
    xSemaphoreGive(s_session_lock);
    return err;
}

esp_err_t screen_adapter_restart_prepared_score(void)
{
    scoring_service_status_t scoring = {0};
    scoring_service_get_status(&scoring);
    if (s_session_lock == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if (s_result_task_running) {
        if (scoring.state == SCORING_SERVICE_READY) {
            s_result_action = RESULT_ACTION_RESTART;
            xSemaphoreGive(s_session_lock);
            set_music_status("\xE6\xAD\xA3\xE5\x9C\xA8\xE9\x87\x8D\xE6\x96\xB0\xE5\xBC\x80\xE5\xA7\x8B...");
            for (size_t attempt = 0; attempt < 100U; ++attempt) {
                vTaskDelay(pdMS_TO_TICKS(10));
                scoring_service_get_status(&scoring);
                if (scoring.state == SCORING_SERVICE_RECORDING ||
                    scoring.state == SCORING_SERVICE_PAUSED) {
                    return ESP_OK;
                }
                xSemaphoreTake(s_session_lock, portMAX_DELAY);
                const bool result_running = s_result_task_running;
                xSemaphoreGive(s_session_lock);
                if (!result_running) {
                    return screen_adapter_start_prepared_score();
                }
            }
            return ESP_ERR_TIMEOUT;
        }
        xSemaphoreGive(s_session_lock);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_session_lock);
    return screen_adapter_start_prepared_score();
}

void screen_adapter_get_preparation_status(
    screen_preparation_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    if (s_session_lock != NULL) {
        xSemaphoreTake(s_session_lock, portMAX_DELAY);
        *status = s_preparation;
        xSemaphoreGive(s_session_lock);
    }
    status->screen_ready = s_ready;
}

static const char *voice_command_text(uint8_t command_id)
{
    switch (command_id) {
    case 1: return "开始练习";
    case 2: return "停止练习";
    case 3: return "下一页";
    case 4: return "上一页";
    case 5: return "重新开始";
    case 6: return "查看评分";
    case 7: return "返回首页";
    default: return "未知指令";
    }
}

static bool voice_page_turn(music_display_page_direction_t direction)
{
    if (!guider_ui.music_screen ||
        lv_screen_active() != guider_ui.music_screen ||
        creator_mode_is_active() || s_session_lock == NULL) {
        return false;
    }

    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    bool active = s_session_active;
    bool paused = s_session_paused;
    bool preview = s_preparation.phase == SCREEN_PREPARATION_READING;
    xSemaphoreGive(s_session_lock);
    if (!preview && !(active && paused)) return false;

    return music_display_request_page_turn(
        direction, MUSIC_DISPLAY_PAGE_SOURCE_VOICE);
}

static bool voice_restart_practice(void)
{
    if (creator_mode_is_active() || s_session_lock == NULL) return false;

    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    bool active = s_session_active;
    xSemaphoreGive(s_session_lock);
    if (active) return begin_practice_completion(RESULT_ACTION_RESTART);

    if (s_result_task_running &&
        (lv_screen_active() == guider_ui.music_screen ||
         lv_screen_active() == guider_ui.end_screen)) {
        s_result_action = RESULT_ACTION_RESTART;
        set_music_status("\xE6\xAD\xA3\xE5\x9C\xA8\xE9\x87\x8D\xE6\x96\xB0\xE5\xBC\x80\xE5\xA7\x8B...");
        return true;
    }

    if (!guider_ui.end_screen || lv_screen_active() != guider_ui.end_screen) {
        return false;
    }
    esp_err_t err = scoring_service_reset();
    if (err != ESP_OK) return false;
    return screen_adapter_start_prepared_score() == ESP_OK;
}

static bool voice_go_home(void)
{
    if (!guider_ui.screen || creator_mode_is_active()) return false;

    lv_obj_t *active_screen = lv_screen_active();
    bool supported = active_screen == guider_ui.screen ||
                     active_screen == guider_ui.choose_screen ||
                     active_screen == guider_ui.music_preparation ||
                     active_screen == guider_ui.music_screen ||
                     active_screen == guider_ui.end_screen ||
                     active_screen == guider_ui.setting_screen;
    if (!supported) return false;

    if (active_screen != guider_ui.screen) {
        bool practice_active = false;
        if (s_session_lock != NULL) {
            xSemaphoreTake(s_session_lock, portMAX_DELAY);
            practice_active = s_session_active;
            xSemaphoreGive(s_session_lock);
        }
        if (practice_active) {
            if (!begin_practice_completion(RESULT_ACTION_RESET)) return false;
        } else if (s_result_task_running) {
            s_result_action = RESULT_ACTION_RESET;
        } else {
            scoring_service_status_t status = {0};
            scoring_service_get_status(&status);
            if (status.state == SCORING_SERVICE_READY ||
                status.state == SCORING_SERVICE_ERROR) {
                (void)scoring_service_reset();
            } else if (status.state == SCORING_SERVICE_SCORING) {
                return false;
            }
        }
    }

    hide_practice_buttons();
    music_display_set_practice_navigation_state(false, false);
    music_display_set_read_only(false);
    if (s_result_back_button && lv_obj_is_valid(s_result_back_button)) {
        lv_obj_add_flag(s_result_back_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (active_screen != guider_ui.screen) {
        lv_screen_load_anim(guider_ui.screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT,
                            180, 0, false);
    }
    return true;
}

static bool execute_voice_command(uint8_t command_id)
{
    switch (command_id) {
    case 1:
        if (!guider_ui.music_preparation ||
            lv_screen_active() != guider_ui.music_preparation) {
            return false;
        }
        return screen_adapter_start_prepared_score() == ESP_OK;
    case 2:
        return pause_active_practice();
    case 3:
        return voice_page_turn(MUSIC_DISPLAY_PAGE_NEXT);
    case 4:
        return voice_page_turn(MUSIC_DISPLAY_PAGE_PREVIOUS);
    case 5:
        return voice_restart_practice();
    case 6:
        if (guider_ui.end_screen &&
            lv_screen_active() == guider_ui.end_screen) {
            return true;
        }
        if (!guider_ui.music_screen ||
            lv_screen_active() != guider_ui.music_screen) {
            return false;
        }
        return begin_practice_completion(RESULT_ACTION_SHOW);
    case 7:
        return voice_go_home();
    default:
        return false;
    }
}

static void voice_ui_event_async_cb(void *user_data)
{
    voice_ui_event_t *event = user_data;
    if (event == NULL) return;

    if (event->type == SCREEN_VOICE_EVENT_WAKE) {
        show_voice_popup("正在聆听…\n请说出指令", true,
                         SCREEN_VOICE_LISTEN_TIMEOUT_MS);
    } else if (event->type == SCREEN_VOICE_EVENT_TIMEOUT) {
        hide_voice_popup();
    } else if (event->type == SCREEN_VOICE_EVENT_COMMAND) {
        const char *command_text = voice_command_text(event->command_id);
        bool handled = execute_voice_command(event->command_id);
        char feedback[96];
        snprintf(feedback, sizeof(feedback),
                 handled ? "已执行：%s" : "当前页面无法执行\n%s",
                 command_text);
        show_voice_popup(feedback, handled,
                         SCREEN_VOICE_FEEDBACK_TIMEOUT_MS);
        if (event->result_callback != NULL) {
            event->result_callback(event->command_id, handled,
                                   event->result_context);
        }
    }
    free(event);
}

esp_err_t screen_adapter_handle_voice_event(
    screen_voice_event_type_t type,
    uint8_t command_id,
    screen_voice_command_result_cb_t result_callback,
    void *result_context)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (type != SCREEN_VOICE_EVENT_WAKE &&
        type != SCREEN_VOICE_EVENT_TIMEOUT &&
        type != SCREEN_VOICE_EVENT_COMMAND) {
        return ESP_ERR_INVALID_ARG;
    }
    if (type == SCREEN_VOICE_EVENT_COMMAND &&
        (command_id < 1 || command_id > 7)) {
        return ESP_ERR_INVALID_ARG;
    }

    voice_ui_event_t *event = calloc(1, sizeof(*event));
    if (event == NULL) return ESP_ERR_NO_MEM;
    event->type = type;
    event->command_id = command_id;
    event->result_callback = result_callback;
    event->result_context = result_context;

    lv_result_t queued = LV_RESULT_INVALID;
    if (bsp_display_lock(portMAX_DELAY)) {
        queued = lv_async_call(voice_ui_event_async_cb, event);
        bsp_display_unlock();
    }
    if (queued != LV_RESULT_OK) {
        free(event);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t screen_adapter_creator_start(
    const screen_creator_config_t *config)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (!config) return ESP_ERR_INVALID_ARG;

    scoring_service_status_t scoring = {0};
    scoring_service_get_status(&scoring);
    if (scoring.state == SCORING_SERVICE_RECORDING ||
        scoring.state == SCORING_SERVICE_PAUSED ||
        scoring.state == SCORING_SERVICE_SCORING)
        return ESP_ERR_INVALID_STATE;

    creator_recorder_config_t creator_config = {
        .bpm = config->bpm,
        .time_sig_num = config->time_sig_num,
        .time_sig_den = config->time_sig_den,
        .staff_mode = config->staff_mode == SCREEN_CREATOR_STAFF_GRAND
                          ? CREATOR_STAFF_GRAND
                          : CREATOR_STAFF_SINGLE,
    };
    return creator_mode_start(&creator_config);
}

esp_err_t screen_adapter_creator_pause(void)
{
    return s_ready ? creator_mode_pause() : ESP_ERR_INVALID_STATE;
}

esp_err_t screen_adapter_creator_resume(void)
{
    return s_ready ? creator_mode_resume() : ESP_ERR_INVALID_STATE;
}

esp_err_t screen_adapter_creator_finish(void)
{
    return s_ready ? creator_mode_finish() : ESP_ERR_INVALID_STATE;
}

esp_err_t screen_adapter_creator_cancel(void)
{
    return s_ready ? creator_mode_cancel() : ESP_ERR_INVALID_STATE;
}

void screen_adapter_creator_get_status(screen_creator_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));

    creator_mode_status_t creator = {0};
    creator_mode_get_status(&creator);
    status->available = s_ready && creator.available;
    status->active = creator.active;
    status->waiting_first_note = creator.waiting_first_note;
    status->state = (screen_creator_state_t)creator.state;
    status->config.bpm = creator.config.bpm;
    status->config.time_sig_num = creator.config.time_sig_num;
    status->config.time_sig_den = creator.config.time_sig_den;
    status->config.staff_mode =
        creator.config.staff_mode == CREATOR_STAFF_GRAND
            ? SCREEN_CREATOR_STAFF_GRAND
            : SCREEN_CREATOR_STAFF_SINGLE;
    status->note_count = creator.note_count;
    status->measure_count = creator.measure_count;
    status->last_error = creator.last_error;
    snprintf(status->saved_title, sizeof(status->saved_title), "%s",
             creator.saved_title);
    snprintf(status->saved_filename, sizeof(status->saved_filename), "%s",
             creator.saved_filename);
    snprintf(status->message, sizeof(status->message), "%s",
             creator.message);

    usb_midi_status_t usb = {0};
    usb_midi_get_status(&usb);
    status->usb_midi_connected = usb.connected;
}

static void handle_practice_note_on(const usb_midi_event_t *event)
{
    int target_slot = 0;
    int expected_midi = 0;
    bool pitch_ok = false;
    bool onset_ok = false;
    int64_t page_score_time_ms = -1;
    settled_note_result_t settled[SCREEN_MATCH_WINDOW_SIZE];
    size_t settled_count = 0;

    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if (s_session_active && !s_session_paused &&
        s_aligned_index < s_target_score.note_count &&
        s_aligned_index < SCORE_DATA_MAX_NOTES) {
        const score_note_t *search_start =
            &s_target_score.notes[s_aligned_index];
        int64_t expected_start_us =
            s_performance_origin_us == 0
                ? (int64_t)event->timestamp_us
                : s_performance_origin_us +
                      scaled_target_time_us(
                          (int64_t)search_start->start_ms * 1000LL);
        const int pitch_tolerance =
            s_session_input == INPUT_SOURCE_AUDIO_S3 &&
                    s_first_match_pending
                ? SCREEN_AUDIO_FIRST_NOTE_PITCH_TOLERANCE
                : SCREEN_FOLLOW_MATCH_PITCH_TOLERANCE;
        int candidate_score = 0;
        int candidate = find_best_match_in_window(
            s_target_score.notes, s_target_score.note_count,
            (int)s_aligned_index, SCREEN_MATCH_WINDOW_SIZE,
            event->midi, (int64_t)event->timestamp_us,
            expected_start_us, pitch_tolerance, &candidate_score);

        if (candidate >= 0) {
            if (s_session_input == INPUT_SOURCE_AUDIO_S3) {
                s_first_match_pending = false;
            }
            size_t candidate_index = (size_t)candidate;
            const score_note_t *target =
                &s_target_score.notes[candidate_index];
            int64_t target_onset_us =
                (int64_t)target->start_ms * 1000LL;
            if (s_performance_origin_us == 0) {
                s_performance_origin_us =
                    (int64_t)event->timestamp_us -
                    scaled_target_time_us(target_onset_us);
            }

            int64_t expected_onset_us =
                s_performance_origin_us +
                scaled_target_time_us(target_onset_us);
            int64_t onset_error_us =
                (int64_t)event->timestamp_us - expected_onset_us;
            if (onset_error_us < 0) onset_error_us = -onset_error_us;
            onset_ok = onset_error_us <=
                       (int64_t)SCREEN_RHYTHM_TOLERANCE_MS * 1000LL;
            pitch_ok =
                midi_distance(target->midi, event->midi) <=
                SCREEN_DISPLAY_GREEN_PITCH_TOLERANCE;

            settled_count = consume_matched_target_locked(
                candidate_index, settled, SCREEN_MATCH_WINDOW_SIZE);
            add_pending_note_locked(candidate_index, event,
                                    pitch_ok, onset_ok);
            target_slot = candidate + 1;
            expected_midi = target->midi;

            ESP_LOGD(TAG,
                     "live match played=%u expected=%d target=%d score=%d pitch_ok=%d error=%lldms tempo=%.3f",
                     event->midi, expected_midi, target_slot,
                     candidate_score, pitch_ok,
                     (long long)(onset_error_us / 1000LL),
                     (double)s_tempo_ema);
        }
    }
    page_score_time_ms =
        current_score_time_ms_locked((int64_t)event->timestamp_us);
    if (target_slot == 0 && page_score_time_ms >= 0) {
        remember_extra_observation_locked(event->midi, page_score_time_ms);
    }
    xSemaphoreGive(s_session_lock);

    apply_settled_results(settled, settled_count);
    if (target_slot > 0) {
        music_display_apply_note_result(target_slot, expected_midi,
                                        event->midi, 1.0f,
                                        pitch_ok, onset_ok);
    }
    if (page_score_time_ms >= 0) {
        music_display_check_time_page_turn(page_score_time_ms);
    }
}

static void handle_practice_note_off(const usb_midi_event_t *event)
{
    pending_note_t completed = {0};
    bool found = false;
    bool duration_ok = false;
    int64_t page_score_time_ms = -1;

    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if (s_session_active && !s_session_paused) {
        int pending_index =
            find_pending_note_locked(event->midi, event->channel);
        if (pending_index >= 0) {
            completed = s_pending_notes[pending_index];
            s_pending_notes[pending_index].active = false;
            found = true;

            int64_t played_duration_us =
                (int64_t)event->timestamp_us - completed.played_onset_us;
            int64_t expected_duration_us =
                scaled_target_time_us(completed.target_duration_us);
            if (expected_duration_us < 1000LL) expected_duration_us = 1000LL;
            int64_t duration_error_us =
                played_duration_us - expected_duration_us;
            if (duration_error_us < 0) duration_error_us = -duration_error_us;
            duration_ok =
                played_duration_us > 0 &&
                (double)duration_error_us <=
                    (double)expected_duration_us *
                        SCREEN_DURATION_TOLERANCE_RATIO;

            if (completed.pitch_ok && duration_ok) {
                update_tempo_estimate(
                    completed.played_onset_us,
                    completed.target_onset_us,
                    played_duration_us,
                    completed.target_duration_us);
            }

            ESP_LOGD(TAG,
                     "duration refine target=%u played=%lldms expected=%lldms ok=%d tempo=%.3f",
                     (unsigned)(completed.target_index + 1U),
                     (long long)(played_duration_us / 1000LL),
                     (long long)(expected_duration_us / 1000LL),
                     duration_ok, (double)s_tempo_ema);
        }

        page_score_time_ms =
            current_score_time_ms_locked((int64_t)event->timestamp_us);
    }
    xSemaphoreGive(s_session_lock);

    if (found) {
        music_display_apply_note_result(
            (int)completed.target_index + 1,
            completed.expected_note, completed.played_note, 1.0f,
            completed.pitch_ok,
            completed.onset_ok && duration_ok);
    }
    if (page_score_time_ms >= 0) {
        music_display_check_time_page_turn(page_score_time_ms);
    }
}

void screen_adapter_handle_usb_midi_event(const usb_midi_event_t *event)
{
    if (event == NULL || !s_ready) return;

    usb_midi_input_event_t legacy = {
        .timestamp_us = event->timestamp_us,
        .cable = event->cable,
        .channel = event->channel,
        .note = event->midi,
        .velocity = event->velocity,
    };
    switch (event->type) {
    case USB_MIDI_EVENT_CONNECTED: {
        legacy.type = USB_MIDI_INPUT_DEVICE_CONNECTED;
        usb_midi_status_t status;
        usb_midi_get_status(&status);
        legacy.vid = status.vid;
        legacy.pid = status.pid;
        strlcpy(legacy.product, status.product, sizeof(legacy.product));
        break;
    }
    case USB_MIDI_EVENT_DISCONNECTED:
        legacy.type = USB_MIDI_INPUT_DEVICE_DISCONNECTED;
        break;
    case USB_MIDI_EVENT_NOTE_ON:
        legacy.type = event->velocity == 0
                          ? USB_MIDI_INPUT_NOTE_OFF
                          : USB_MIDI_INPUT_NOTE_ON;
        break;
    case USB_MIDI_EVENT_NOTE_OFF:
        legacy.type = USB_MIDI_INPUT_NOTE_OFF;
        break;
    default:
        return;
    }
    if (creator_mode_handle_midi_event(&legacy)) return;
    if (s_session_lock == NULL) return;

    bool note_off = event->type == USB_MIDI_EVENT_NOTE_OFF ||
                    (event->type == USB_MIDI_EVENT_NOTE_ON &&
                     event->velocity == 0);
    if (note_off) {
        handle_practice_note_off(event);
    } else if (event->type == USB_MIDI_EVENT_NOTE_ON) {
        handle_practice_note_on(event);
    }
}
