#include "music_detector.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "adaptive_input_control.h"
#include "audio_preprocess.h"
#include "chord_detector.h"
#include "diagnostics.h"
#include "dual_mic_selector.h"
#include "es7210_capture.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "low_note_detector.h"
#include "music_classifier.h"
#include "music_detector_config.h"
#include "music_uart_link.h"
#include "note_utils.h"
#include "piano_note_tracker.h"
#include "yin_detector.h"

typedef struct {
    yin_result_t history[MUSIC_LOW_YIN_HISTORY_FRAMES];
    uint8_t next;
} low_yin_stabilizer_t;

#if MUSIC_USE_SINGLE_MIC_CH1
static float s_audio_ring[MUSIC_FFT_SIZE];
static float s_frame_float[MUSIC_CAPTURE_FRAMES];
#else
static float s_audio_ring[2][MUSIC_FFT_SIZE];
static float s_frame_float[2][MUSIC_CAPTURE_FRAMES];
#endif
static float s_yin_window[MUSIC_YIN_WINDOW_SIZE];
static float s_low_yin_window[MUSIC_LOW_YIN_WINDOW_SIZE];
static low_note_detector_t s_low_note_detector;
static low_yin_stabilizer_t s_low_yin_stabilizer;
static piano_note_tracker_t s_note_tracker;
static size_t s_ring_write_position;
static size_t s_ring_filled;
static int s_bass_midi = -1;
static int s_bass_pending_midi = -1;
static unsigned s_bass_pending_frames = 0;
static unsigned s_bass_missing_frames = 0;
static bool s_bass_injected = false;
static volatile music_recognition_profile_t s_recognition_profile =
    MUSIC_RECOGNITION_PROFILE_STRICT;

static uint8_t low_fallback_velocity(float rms)
{
    const float level = fminf(fmaxf(rms, 0.0f) / PIANO_TRACKER_VELOCITY_RMS,
                              1.0f);
    const int velocity = (int)lrintf(127.0f * sqrtf(level));
    return (uint8_t)(velocity < 1 ? 1 : (velocity > 127 ? 127 : velocity));
}

static bool bass_attack_supported(const low_note_result_t *low_match,
                                  const yin_result_t *low_yin, int *midi)
{
    *midi = -1;
    if (low_yin != NULL && low_yin->valid &&
        low_yin->confidence >= MUSIC_LOW_CHORD_YIN_CONFIDENCE &&
        low_yin->midi >= MUSIC_PIANO_MIDI_MIN &&
        low_yin->midi <= MUSIC_VIRTUAL_FUNDAMENTAL_MAX_MIDI) {
        *midi = low_yin->midi;
        return true;
    }
    if (low_match != NULL && low_match->valid && low_match->count == 1U &&
        low_match->confidence[0] >= PIANO_TRACKER_SINGLE_MIN_CONFIDENCE) {
        *midi = low_match->midi[0];
        return true;
    }
    return false;
}

static bool bass_hold_supported(const low_note_result_t *low_match,
                                const yin_result_t *low_yin, int midi)
{
    if (low_yin != NULL && low_yin->valid && low_yin->midi == midi &&
        low_yin->confidence >= 0.70f) {
        return true;
    }
    return low_match != NULL && low_match->valid &&
        low_match->count == 1U && low_match->midi[0] == midi &&
        low_match->confidence[0] >= MUSIC_LOW_MATCH_SECONDARY_CONFIDENCE;
}

static void invalidate_yin(yin_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->midi = -1;
}

static void low_yin_stabilizer_reset(low_yin_stabilizer_t *stabilizer)
{
    memset(stabilizer, 0, sizeof(*stabilizer));
    for (uint8_t index = 0; index < MUSIC_LOW_YIN_HISTORY_FRAMES; ++index) {
        stabilizer->history[index].midi = -1;
    }
}

static float median_observation(float values[MUSIC_LOW_YIN_HISTORY_FRAMES],
                                uint8_t count)
{
    for (uint8_t index = 1; index < count; ++index) {
        const float value = values[index];
        uint8_t move = index;
        while (move > 0 && values[move - 1] > value) {
            values[move] = values[move - 1];
            --move;
        }
        values[move] = value;
    }
    if ((count & 1U) != 0) return values[count / 2U];
    return 0.5f * (values[count / 2U - 1U] + values[count / 2U]);
}

static void low_yin_stabilizer_update(
    low_yin_stabilizer_t *stabilizer, const yin_result_t *observation,
    bool signal_active, yin_result_t *result)
{
    invalidate_yin(result);
    if (!signal_active) {
        low_yin_stabilizer_reset(stabilizer);
        return;
    }

    yin_result_t sample;
    if (observation != NULL && observation->valid &&
        observation->midi >= MUSIC_PIANO_MIDI_MIN &&
        observation->midi <= MUSIC_VIRTUAL_FUNDAMENTAL_MAX_MIDI) {
        sample = *observation;
    } else {
        invalidate_yin(&sample);
    }
    stabilizer->history[stabilizer->next] = sample;
    stabilizer->next = (uint8_t)((stabilizer->next + 1U) %
                                 MUSIC_LOW_YIN_HISTORY_FRAMES);

    int best_midi = -1;
    uint8_t best_votes = 0;
    for (uint8_t index = 0; index < MUSIC_LOW_YIN_HISTORY_FRAMES; ++index) {
        const yin_result_t *candidate = &stabilizer->history[index];
        if (!candidate->valid) continue;
        uint8_t votes = 0;
        for (uint8_t other = 0; other < MUSIC_LOW_YIN_HISTORY_FRAMES; ++other) {
            if (stabilizer->history[other].valid &&
                stabilizer->history[other].midi == candidate->midi) {
                ++votes;
            }
        }
        if (votes > best_votes) {
            best_votes = votes;
            best_midi = candidate->midi;
        }
    }
    if (best_votes < MUSIC_LOW_YIN_STABLE_VOTES) return;

    float frequencies[MUSIC_LOW_YIN_HISTORY_FRAMES];
    float confidences[MUSIC_LOW_YIN_HISTORY_FRAMES];
    uint8_t count = 0;
    for (uint8_t index = 0; index < MUSIC_LOW_YIN_HISTORY_FRAMES; ++index) {
        const yin_result_t *candidate = &stabilizer->history[index];
        if (!candidate->valid || candidate->midi != best_midi) continue;
        frequencies[count] = candidate->frequency_hz;
        confidences[count] = candidate->confidence;
        ++count;
    }
    result->valid = true;
    result->midi = best_midi;
    result->frequency_hz = median_observation(frequencies, count);
    result->confidence = median_observation(confidences, count);
    result->cents = note_cents_error(result->frequency_hz, result->midi);
    note_midi_to_name(result->midi, result->note_name,
                      sizeof(result->note_name));
}

static void promote_low_match_yin(const low_note_result_t *match,
                                  yin_result_t *low_yin)
{
    if (match == NULL || low_yin == NULL || !match->valid ||
        match->count == 0U ||
        match->confidence[0] < PIANO_TRACKER_SINGLE_MIN_CONFIDENCE ||
        (low_yin->valid &&
         low_yin->confidence >= match->confidence[0])) {
        return;
    }
    low_yin->valid = true;
    low_yin->midi = match->midi[0];
    low_yin->frequency_hz = match->frequency_hz[0];
    low_yin->confidence = match->confidence[0];
    low_yin->cents = 0.0f;
    note_midi_to_name(low_yin->midi, low_yin->note_name,
                      sizeof(low_yin->note_name));
}

static float low_match_confidence_for_midi(const low_note_result_t *match,
                                           int midi)
{
    if (match == NULL || !match->valid) return 0.0f;
    for (uint8_t index = 0; index < match->count; ++index) {
        if (match->midi[index] == midi) {
            return match->confidence[index];
        }
    }
    return 0.0f;
}

static int suppress_unconfirmed_low_yin_under_high_note(
    const yin_result_t *high_yin, yin_result_t *low_yin,
    const low_note_result_t *low_match)
{
    if (high_yin == NULL || !high_yin->valid ||
        high_yin->confidence < MUSIC_YIN_CONFIDENCE_THRESHOLD ||
        high_yin->midi <= MUSIC_VIRTUAL_FUNDAMENTAL_MAX_MIDI ||
        low_yin == NULL || !low_yin->valid ||
        low_yin->midi < MUSIC_PIANO_MIDI_MIN ||
        low_yin->midi > MUSIC_VIRTUAL_FUNDAMENTAL_MAX_MIDI) {
        return -1;
    }

    /* The low-rate YIN search stops at 255 Hz. For C4 and above, an otherwise
     * clean monophonic tone therefore exposes its second/third period inside
     * the low search range and looks like a very confident C3/D3/etc. Keep a
     * simultaneous bass only when the independent Goertzel matcher confirms
     * that exact physical low key. */
    if (low_match_confidence_for_midi(low_match, low_yin->midi) >=
        MUSIC_LOW_MATCH_SECONDARY_CONFIDENCE) {
        return -1;
    }

    const int suppressed_midi = low_yin->midi;
    invalidate_yin(low_yin);
    return suppressed_midi;
}

void music_detector_set_recognition_profile(music_recognition_profile_t profile)
{
    if (profile != MUSIC_RECOGNITION_PROFILE_DEMO) {
        profile = MUSIC_RECOGNITION_PROFILE_STRICT;
    }
    const music_recognition_profile_t previous = s_recognition_profile;
    s_recognition_profile = profile;
    if (previous != profile) {
        ESP_LOGI("MUSIC", "recognition profile: %s",
                 profile == MUSIC_RECOGNITION_PROFILE_DEMO ? "demo" : "strict");
    }
}

music_recognition_profile_t music_detector_get_recognition_profile(void)
{
    return s_recognition_profile == MUSIC_RECOGNITION_PROFILE_DEMO
               ? MUSIC_RECOGNITION_PROFILE_DEMO
               : MUSIC_RECOGNITION_PROFILE_STRICT;
}

static float signal_snr_db(const audio_frame_metrics_t *metrics,
                           const audio_preprocess_state_t *preprocess)
{
    const float reference = fmaxf(preprocess->noise_floor,
                                  MUSIC_MIN_RMS * 0.25f);
    const float ratio = metrics->rms / fmaxf(reference, 1.0e-7f);
    return ratio > 1.0e-6f ? 20.0f * log10f(ratio) : -120.0f;
}

static void reset_spectral_history(void)
{
    memset(s_audio_ring, 0, sizeof(s_audio_ring));
    memset(s_yin_window, 0, sizeof(s_yin_window));
    memset(s_low_yin_window, 0, sizeof(s_low_yin_window));
    s_ring_write_position = 0;
    s_ring_filled = 0;
    s_bass_midi = -1;
    s_bass_pending_midi = -1;
    s_bass_pending_frames = 0;
    s_bass_missing_frames = 0;
    s_bass_injected = false;
    low_note_detector_reset(&s_low_note_detector);
    low_yin_stabilizer_reset(&s_low_yin_stabilizer);
}

static bool copy_low_yin_window(int selected_mic)
{
    return low_note_detector_copy_recent(
        &s_low_note_detector, selected_mic - 1, s_low_yin_window,
        MUSIC_LOW_YIN_WINDOW_SIZE);
}

static void copy_yin_window(int selected_mic)
{
#if MUSIC_USE_SINGLE_MIC_CH1
    (void)selected_mic;
    const float *ring = s_audio_ring;
#else
    const float *ring = s_audio_ring[selected_mic - 1];
#endif
    const size_t start = (s_ring_write_position + MUSIC_FFT_SIZE - MUSIC_YIN_WINDOW_SIZE) % MUSIC_FFT_SIZE;
    for (size_t i = 0; i < MUSIC_YIN_WINDOW_SIZE; ++i) {
        s_yin_window[i] = ring[(start + i) % MUSIC_FFT_SIZE];
    }
}

#if !MUSIC_USE_SINGLE_MIC_CH1
static int update_selected_mic(int selected, int *challenger_count,
                               const audio_frame_metrics_t metrics[2],
                               const audio_preprocess_state_t state[2])
{
    float score[2];
    for (int mic = 0; mic < 2; ++mic) {
        const float snr_proxy = metrics[mic].rms / fmaxf(state[mic].noise_gate, 1.0e-6f);
        score[mic] = snr_proxy / (snr_proxy + 1.0f);
        if (metrics[mic].clipped) score[mic] *= 0.15f;
    }
    const int current = selected - 1;
    const int other = 1 - current;
    if (score[other] > score[current] + MUSIC_MIC_SWITCH_SCORE_MARGIN) {
        ++*challenger_count;
        if (*challenger_count >= MUSIC_MIC_SWITCH_CONFIRM_FRAMES) {
            *challenger_count = 0;
            return other + 1;
        }
    } else {
        *challenger_count = 0;
    }
    return selected;
}
#endif

static void log_result(const music_result_t *result, const char *unknown_reason,
                       const chord_result_t *chord)
{
    switch (result->type) {
        case MUSIC_RESULT_SILENCE:
#if MUSIC_USE_SINGLE_MIC_CH1
            ESP_LOGI("RESULT", "SILENCE mic1=%.5f", result->mic1_rms);
#else
            ESP_LOGI("RESULT", "SILENCE mic1=%.5f mic2=%.5f", result->mic1_rms, result->mic2_rms);
#endif
            break;
        case MUSIC_RESULT_SINGLE:
            ESP_LOGI("RESULT", "SINGLE note=%s midi=%d freq=%.1fHz cents=%+.1f conf=%.2f mic=%d harmonic_ratio=%.2f",
                     result->note_name, result->midi, result->frequency_hz, result->cents,
                     result->confidence, result->selected_mic, result->harmonic_explained_ratio);
            ESP_LOGI("DETECTED_MUSIC", "SINGLE [%s]", result->note_name);
            break;
        case MUSIC_RESULT_INTERVAL: {
            char first_note[8] = "?";
            char second_note[8] = "?";
            if (result->midi_notes[0] >= 0) {
                note_midi_to_name(result->midi_notes[0], first_note, sizeof(first_note));
            }
            if (result->midi_notes[1] >= 0) {
                note_midi_to_name(result->midi_notes[1], second_note, sizeof(second_note));
            }
#if MUSIC_USE_SINGLE_MIC_CH1
            ESP_LOGI("RESULT", "INTERVAL notes=[%s,%s] pitch_classes=[%s,%s] midi=[%d,%d] conf=%.2f mic=1",
#else
            ESP_LOGI("RESULT", "INTERVAL notes=[%s,%s] pitch_classes=[%s,%s] midi=[%d,%d] conf=%.2f mic_fusion=1",
#endif
                     first_note, second_note,
                     note_pitch_class_name(result->pitch_classes[0]),
                     note_pitch_class_name(result->pitch_classes[1]),
                     result->midi_notes[0], result->midi_notes[1], result->confidence);
            ESP_LOGI("DETECTED_MUSIC", "NOTES [%s + %s]", first_note, second_note);
            break;
        }
        case MUSIC_RESULT_CHORD: {
            char first_note[8] = "?";
            char second_note[8] = "?";
            char third_note[8] = "?";
            if (result->midi_notes[0] >= 0) {
                note_midi_to_name(result->midi_notes[0], first_note, sizeof(first_note));
            }
            if (result->midi_notes[1] >= 0) {
                note_midi_to_name(result->midi_notes[1], second_note, sizeof(second_note));
            }
            if (result->midi_notes[2] >= 0) {
                note_midi_to_name(result->midi_notes[2], third_note, sizeof(third_note));
            }
#if MUSIC_USE_SINGLE_MIC_CH1
            ESP_LOGI("RESULT", "CHORD chord=%s notes=[%s,%s,%s] conf=%.2f mic=1",
#else
            ESP_LOGI("RESULT", "CHORD chord=%s notes=[%s,%s,%s] conf=%.2f mic_fusion=1",
#endif
                     result->chord_name, note_pitch_class_name(result->pitch_classes[0]),
                     note_pitch_class_name(result->pitch_classes[1]),
                     note_pitch_class_name(result->pitch_classes[2]), result->confidence);
            ESP_LOGI("DETECTED_MUSIC", "CHORD %s [%s + %s + %s]",
                     result->chord_name, first_note, second_note, third_note);
            break;
        }
        case MUSIC_RESULT_UNKNOWN:
        default:
            ESP_LOGI("RESULT", "UNKNOWN rms=%.5f yin_note=%s midi=%d freq=%.1fHz yin=%.2f harmonic=%.2f pitch_classes=%d poly_conf=%.2f reason=%s",
                     result->rms, result->midi >= 0 ? result->note_name : "-", result->midi,
                     result->frequency_hz, result->yin_confidence,
                     result->harmonic_explained_ratio, chord->independent_pitch_class_count,
                     chord->confidence, unknown_reason);
            break;
    }
}

static void log_exact_note_set(const piano_note_set_t *set)
{
    char names[48] = {0};
    size_t used = 0;
    for (uint8_t index = 0; index < set->count; ++index) {
        char name[8];
        note_midi_to_name(set->midi[index], name, sizeof(name));
        const int written = snprintf(names + used, sizeof(names) - used,
                                     index == 0 ? "%s" : "+%s", name);
        if (written < 0 || (size_t)written >= sizeof(names) - used) break;
        used += (size_t)written;
    }
    ESP_LOGI("PIANO_KEYS",
             "count=%u notes=[%s] degraded_mic=%s overflow=%s ts=%" PRIu32,
             (unsigned)set->count, names,
             set->degraded_mic ? "true" : "false",
             set->overflow ? "true" : "false", set->timestamp_ms);
}

static void log_spectrum_debug(const chord_result_t *spectrum)
{
    char entries[3][48];
    for (int i = 0; i < 3; ++i) {
        if (i >= spectrum->debug_candidate_count) {
            snprintf(entries[i], sizeof(entries[i]), "-");
            continue;
        }
        const chord_candidate_debug_t *candidate = &spectrum->debug_candidates[i];
        char note_name[8] = "?";
        note_midi_to_name(candidate->midi, note_name, sizeof(note_name));
        snprintf(entries[i], sizeof(entries[i]), "%s@%.1f/b%d/r%.2f/p%.2f",
                 note_name, candidate->peak_frequency_hz, candidate->peak_bin,
                 candidate->relative_score, candidate->prominence);
    }
    ESP_LOGI("SPECTRUM_DEBUG", "active=%d noise=%.6f bands=[%.6f,%.6f,%.6f] candidates=%d harmonic_rejected=%d top=[%s;%s;%s]",
             spectrum->independent_pitch_class_count, spectrum->spectrum_noise_floor,
             spectrum->band_noise_floor[0], spectrum->band_noise_floor[1],
             spectrum->band_noise_floor[2],
             spectrum->debug_candidate_count, spectrum->harmonic_rejected_count,
             entries[0], entries[1], entries[2]);
}

static const char *adaptive_snr_tier(bool demo_profile, float snr_db)
{
    if (!demo_profile) return "strict";
    if (snr_db >= MUSIC_DEMO_SNR_HIGH_DB) return "high";
    if (snr_db >= MUSIC_DEMO_SNR_MEDIUM_DB) return "medium";
    if (snr_db >= MUSIC_DEMO_SNR_MIN_DB) return "guarded";
    return "reject-poly";
}

static void music_dsp_task(void *argument)
{
    (void)argument;
#if MUSIC_USE_SINGLE_MIC_CH1
    audio_preprocess_state_t preprocess;
#else
    audio_preprocess_state_t preprocess[2];
#endif
    audio_frame_metrics_t metrics[2] = {0};
    music_classifier_t classifier;
#if MUSIC_USE_SINGLE_MIC_CH1
    audio_preprocess_init(&preprocess);
#else
    for (int mic = 0; mic < 2; ++mic) audio_preprocess_init(&preprocess[mic]);
#endif
    music_classifier_init(&classifier);
    adaptive_input_control_t input_control;
    adaptive_input_control_init(&input_control,
                                es7210_capture_get_input_gain());
    music_recognition_profile_t active_profile =
        MUSIC_RECOGNITION_PROFILE_STRICT;
    bool active_profile_initialized = false;
    yin_detector_init();
    low_note_detector_init(&s_low_note_detector);
    low_yin_stabilizer_reset(&s_low_yin_stabilizer);
    piano_note_tracker_init(&s_note_tracker);
    int selected_mic = 1;
    unsigned spectrum_hop_counter = 0;
#if !MUSIC_USE_SINGLE_MIC_CH1
    int challenger_count = 0;
    dual_mic_selector_t demo_selector;
    dual_mic_selector_init(&demo_selector);
    dual_mic_selection_t demo_selection = {0};
#endif
    uint32_t calibration_start_ms = 0;
    uint32_t last_diagnostic_ms = 0;
    uint32_t last_performance_ms = 0;
    uint32_t last_spectrum_debug_ms = 0;
    uint32_t last_low_match_log_ms = 0;
    uint32_t low_peak_diagnostics = 0;
    float selected_snr_db = -120.0f;
    chord_result_t last_spectrum_debug = {0};
#if MUSIC_USE_SINGLE_MIC_CH1
    float last_yin_confidence = 0.0f;
    float last_chord_confidence = 0.0f;
    unsigned active_hangover_blocks = 0;
#endif
    bool calibrated = false;
    diagnostics_counters_t *counters = diagnostics_counters();

    while (true) {
        audio_capture_block_t *block = NULL;
        const int block_index = es7210_capture_take_block(&block, 1000);
        if (block_index < 0) continue;
        const int64_t cycle_start_us = esp_timer_get_time();
        if (calibration_start_ms == 0) {
            calibration_start_ms = block->timestamp_ms;
#if MUSIC_USE_SINGLE_MIC_CH1
            ESP_LOGI("AUDIO", "CH1 calibration started");
#else
            ESP_LOGI("AUDIO", "dual-microphone calibration started");
#endif
        }
        const uint32_t block_timestamp_ms = block->timestamp_ms;
        const music_recognition_profile_t requested_profile =
            music_detector_get_recognition_profile();
        if (!active_profile_initialized || requested_profile != active_profile) {
            if (active_profile_initialized) {
                piano_note_set_t released_set;
                if (piano_note_tracker_release_all(
                        &s_note_tracker, block_timestamp_ms, &released_set)) {
                    music_uart_link_submit_note_set(&released_set);
                }
            }
            active_profile_initialized = true;
            active_profile = requested_profile;
            music_classifier_init(&classifier);
            low_note_detector_reset(&s_low_note_detector);
            low_yin_stabilizer_reset(&s_low_yin_stabilizer);
            s_bass_midi = -1;
            s_bass_pending_midi = -1;
            s_bass_pending_frames = 0;
            s_bass_missing_frames = 0;
            s_bass_injected = false;
            spectrum_hop_counter = 0;
#if !MUSIC_USE_SINGLE_MIC_CH1
            dual_mic_selector_init(&demo_selector);
            challenger_count = 0;
#endif
            ESP_LOGI("MUSIC", "recognition history reset for profile: %s",
                     active_profile == MUSIC_RECOGNITION_PROFILE_DEMO
                         ? "demo" : "strict");
            float requested_gain_db = input_control.current_gain_db;
            adaptive_gain_reason_t gain_reason = ADAPTIVE_GAIN_REASON_NONE;
            if (adaptive_input_control_profile_target(
                    &input_control,
                    active_profile == MUSIC_RECOGNITION_PROFILE_DEMO,
                    &requested_gain_db, &gain_reason)) {
                const float old_gain_db = input_control.current_gain_db;
                const esp_err_t gain_error =
                    es7210_capture_set_input_gain(requested_gain_db);
                if (gain_error == ESP_OK) {
                    const float linear_scale = powf(
                        10.0f, (requested_gain_db - old_gain_db) / 20.0f);
#if MUSIC_USE_SINGLE_MIC_CH1
                    audio_preprocess_rescale_gain(&preprocess, linear_scale);
#else
                    for (int mic = 0; mic < 2; ++mic) {
                        audio_preprocess_rescale_gain(&preprocess[mic],
                                                      linear_scale);
                    }
#endif
                    adaptive_input_control_applied(
                        &input_control, requested_gain_db,
                        block_timestamp_ms);
                    reset_spectral_history();
                    ESP_LOGI("ADAPTIVE_INPUT",
                             "gain %.0f->%.0f dB reason=%s",
                             (double)old_gain_db, (double)requested_gain_db,
                             adaptive_gain_reason_name(gain_reason));
                    es7210_capture_release_block(block_index);
                    continue;
                }
                ESP_LOGE("ADAPTIVE_INPUT",
                         "gain change %.0f->%.0f dB failed: %s",
                         (double)old_gain_db, (double)requested_gain_db,
                         esp_err_to_name(gain_error));
            }
        }
        const bool demo_profile =
            active_profile == MUSIC_RECOGNITION_PROFILE_DEMO;
#if MUSIC_USE_SINGLE_MIC_CH1
        audio_preprocess_frame(&preprocess, block->mic1, s_frame_float,
                               MUSIC_CAPTURE_FRAMES, &metrics[0]);
        selected_mic = 1;
        selected_snr_db = signal_snr_db(&metrics[0], &preprocess);
        const size_t metric_count = 1;
#else
        audio_preprocess_frame(&preprocess[0], block->mic1, s_frame_float[0],
                               MUSIC_CAPTURE_FRAMES, &metrics[0]);
        audio_preprocess_frame(&preprocess[1], block->mic2, s_frame_float[1],
                               MUSIC_CAPTURE_FRAMES, &metrics[1]);
        if (demo_profile) {
            dual_mic_selector_update(&demo_selector, metrics, preprocess,
                                     &demo_selection);
            selected_mic = demo_selection.selected_mic;
            selected_snr_db =
                demo_selector.quality[selected_mic - 1].snr_db;
        } else {
            selected_snr_db = signal_snr_db(&metrics[selected_mic - 1],
                                             &preprocess[selected_mic - 1]);
        }
        const size_t metric_count = 2;
#endif
        if (demo_profile && calibrated) {
            float requested_gain_db = input_control.current_gain_db;
            adaptive_gain_reason_t gain_reason = ADAPTIVE_GAIN_REASON_NONE;
            if (adaptive_input_control_update(
                    &input_control, metrics, metric_count, selected_mic,
                    selected_snr_db, block_timestamp_ms,
                    &requested_gain_db, &gain_reason)) {
                const float old_gain_db = input_control.current_gain_db;
                const audio_frame_metrics_t selected_metrics =
                    metrics[selected_mic - 1];
                const esp_err_t gain_error =
                    es7210_capture_set_input_gain(requested_gain_db);
                if (gain_error == ESP_OK) {
                    const float linear_scale = powf(
                        10.0f, (requested_gain_db - old_gain_db) / 20.0f);
#if MUSIC_USE_SINGLE_MIC_CH1
                    audio_preprocess_rescale_gain(&preprocess, linear_scale);
#else
                    for (int mic = 0; mic < 2; ++mic) {
                        audio_preprocess_rescale_gain(&preprocess[mic],
                                                      linear_scale);
                    }
                    dual_mic_selector_init(&demo_selector);
#endif
                    adaptive_input_control_applied(
                        &input_control, requested_gain_db,
                        block_timestamp_ms);
                    music_classifier_init(&classifier);
                    reset_spectral_history();
                    ESP_LOGI("ADAPTIVE_INPUT",
                             "gain %.0f->%.0f dB reason=%s peak=%.3f clip=%.4f snr=%.1f",
                             (double)old_gain_db, (double)requested_gain_db,
                             adaptive_gain_reason_name(gain_reason),
                             (double)selected_metrics.peak,
                             (double)selected_metrics.clip_rate,
                             (double)selected_snr_db);
                    es7210_capture_release_block(block_index);
                    continue;
                }
                adaptive_input_control_applied(
                    &input_control, old_gain_db, block_timestamp_ms);
                ESP_LOGE("ADAPTIVE_INPUT",
                         "gain change %.0f->%.0f dB reason=%s failed: %s",
                         (double)old_gain_db, (double)requested_gain_db,
                         adaptive_gain_reason_name(gain_reason),
                         esp_err_to_name(gain_error));
            }
        }
#if MUSIC_USE_SINGLE_MIC_CH1
        low_note_detector_push(&s_low_note_detector, 0, s_frame_float,
                               MUSIC_CAPTURE_FRAMES);
        for (size_t i = 0; i < MUSIC_CAPTURE_FRAMES; ++i) {
            const size_t index = (s_ring_write_position + i) % MUSIC_FFT_SIZE;
            s_audio_ring[index] = s_frame_float[i];
        }
#else
        low_note_detector_push(&s_low_note_detector, 0, s_frame_float[0],
                               MUSIC_CAPTURE_FRAMES);
        low_note_detector_push(&s_low_note_detector, 1, s_frame_float[1],
                               MUSIC_CAPTURE_FRAMES);
        for (size_t i = 0; i < MUSIC_CAPTURE_FRAMES; ++i) {
            const size_t index = (s_ring_write_position + i) % MUSIC_FFT_SIZE;
            s_audio_ring[0][index] = s_frame_float[0][i];
            s_audio_ring[1][index] = s_frame_float[1][i];
        }
#endif
        s_ring_write_position = (s_ring_write_position + MUSIC_CAPTURE_FRAMES) % MUSIC_FFT_SIZE;
        if (s_ring_filled < MUSIC_FFT_SIZE) {
            s_ring_filled += MUSIC_CAPTURE_FRAMES;
            if (s_ring_filled > MUSIC_FFT_SIZE) s_ring_filled = MUSIC_FFT_SIZE;
        }
        es7210_capture_release_block(block_index);

        if (!calibrated && block_timestamp_ms - calibration_start_ms >= MUSIC_CALIBRATION_MS) {
#if MUSIC_USE_SINGLE_MIC_CH1
            audio_preprocess_finish_calibration(&preprocess);
            calibrated = true;
            ESP_LOGI("AUDIO", "CH1 calibration finished");
            ESP_LOGI("AUDIO", "ch1_noise_rms=%.5f ch1_noise_gate=%.5f",
                     preprocess.noise_floor, preprocess.noise_gate);
#else
            audio_preprocess_finish_calibration(&preprocess[0]);
            audio_preprocess_finish_calibration(&preprocess[1]);
            calibrated = true;
            ESP_LOGI("AUDIO", "calibration finished");
            ESP_LOGI("AUDIO", "mic1_noise=%.5f mic2_noise=%.5f mic1_gate=%.5f mic2_gate=%.5f",
                     preprocess[0].noise_floor, preprocess[1].noise_floor,
                     preprocess[0].noise_gate, preprocess[1].noise_gate);
#endif
        }

        /* The integrated low-note chain makes the full analysis heavier than
         * one capture block budget. Like the upstream design, run the complete
         * YIN/FFT/chord/classifier pass every MUSIC_SPECTRUM_ANALYSIS_HOPS
         * blocks so the DSP task can block between blocks and never starve
         * the idle task watchdog. Sample pushing keeps running every block. */
        if (calibrated && s_ring_filled >= MUSIC_FFT_SIZE &&
            (++spectrum_hop_counter % MUSIC_SPECTRUM_ANALYSIS_HOPS) == 0) {
            yin_result_t yin = {.midi = -1};
            yin_result_t raw_low_yin = {.midi = -1};
            yin_result_t low_yin = {.midi = -1};
            low_note_result_t low_match = {0};
            chord_result_t chord = {0};
            float harmonic_ratio = 0.0f;
            int suppressed_low_midi = -1;
            bool exact_signal_active = false;
            bool exact_attack_allowed = false;
            float exact_rms = 0.0f;
#if MUSIC_USE_SINGLE_MIC_CH1
            selected_mic = 1;
            const bool above_gate = audio_preprocess_above_gate(&preprocess, &metrics[0]);
            const bool analysis_active = above_gate || active_hangover_blocks > 0;
            if (above_gate) {
                active_hangover_blocks = MUSIC_ACTIVITY_HANGOVER_BLOCKS;
            } else if (active_hangover_blocks > 0) {
                --active_hangover_blocks;
            }
            if (analysis_active) {
                copy_yin_window(selected_mic);
                const bool low_window_ready =
                    copy_low_yin_window(selected_mic);
                int64_t started = esp_timer_get_time();
                yin_detector_analyze(s_yin_window, MUSIC_YIN_WINDOW_SIZE, &yin);
                if (low_window_ready) {
                    yin_detector_analyze_range(
                        s_low_yin_window, MUSIC_LOW_YIN_WINDOW_SIZE,
                        MUSIC_LOW_YIN_SAMPLE_RATE_HZ,
                        MUSIC_LOW_MIN_FREQUENCY_HZ,
                        MUSIC_LOW_YIN_MAX_FREQUENCY_HZ, &raw_low_yin);
                }
                low_yin_stabilizer_update(
                    &s_low_yin_stabilizer, &raw_low_yin, true, &low_yin);
                counters->yin_time_us = (uint32_t)(esp_timer_get_time() - started);
                exact_signal_active = true;
                exact_attack_allowed = above_gate && !metrics[0].clipped;
                exact_rms = metrics[0].rms;
                low_note_detector_analyze(
                    &s_low_note_detector, 0, true, exact_attack_allowed,
                    metrics[0].clipped, &low_match);
                promote_low_match_yin(&low_match, &low_yin);
                suppressed_low_midi =
                    suppress_unconfirmed_low_yin_under_high_note(
                        &yin, &low_yin, &low_match);
                started = esp_timer_get_time();
                chord_detector_analyze(s_audio_ring, NULL, s_ring_write_position,
                                       metrics[0].rms, 0.0f, demo_profile,
                                       selected_snr_db, low_yin.midi,
                                       low_yin.confidence, &low_match,
                                       &chord);
                counters->chord_time_us = (uint32_t)(esp_timer_get_time() - started);
                harmonic_ratio = yin.valid ?
                    chord_detector_harmonic_explained_ratio(yin.frequency_hz) : 0.0f;
            } else {
                low_yin_stabilizer_update(
                    &s_low_yin_stabilizer, NULL, false, &low_yin);
                low_note_detector_analyze(
                    &s_low_note_detector, 0, false, false,
                    metrics[0].clipped, &low_match);
                counters->yin_time_us = 0;
                counters->mic1_fft_time_us = 0;
                counters->mic2_fft_time_us = 0;
                counters->chord_time_us = 0;
            }
            last_yin_confidence = yin.confidence;
            last_chord_confidence = chord.confidence;
#else
            if (!demo_profile) {
                selected_mic = update_selected_mic(
                    selected_mic, &challenger_count, metrics, preprocess);
                selected_snr_db = signal_snr_db(
                    &metrics[selected_mic - 1],
                    &preprocess[selected_mic - 1]);
            }
            copy_yin_window(selected_mic);
            exact_signal_active = audio_preprocess_above_gate(
                &preprocess[selected_mic - 1],
                &metrics[selected_mic - 1]);
            exact_attack_allowed = exact_signal_active &&
                !metrics[selected_mic - 1].clipped;
            exact_rms = metrics[selected_mic - 1].rms;
            int64_t started = esp_timer_get_time();
            yin_detector_analyze(s_yin_window, MUSIC_YIN_WINDOW_SIZE, &yin);
            const bool low_window_ready =
                copy_low_yin_window(selected_mic);
            if (low_window_ready) {
                yin_detector_analyze_range(
                    s_low_yin_window, MUSIC_LOW_YIN_WINDOW_SIZE,
                    MUSIC_LOW_YIN_SAMPLE_RATE_HZ,
                    MUSIC_LOW_MIN_FREQUENCY_HZ,
                    MUSIC_LOW_YIN_MAX_FREQUENCY_HZ, &raw_low_yin);
            }
            low_yin_stabilizer_update(
                &s_low_yin_stabilizer, &raw_low_yin,
                exact_signal_active, &low_yin);
            low_note_detector_analyze(
                &s_low_note_detector, selected_mic - 1,
                exact_signal_active, exact_attack_allowed,
                metrics[selected_mic - 1].clipped, &low_match);
            promote_low_match_yin(&low_match, &low_yin);
            suppressed_low_midi =
                suppress_unconfirmed_low_yin_under_high_note(
                    &yin, &low_yin, &low_match);
            counters->yin_time_us = (uint32_t)(esp_timer_get_time() - started);
            started = esp_timer_get_time();
            const float mic1_weight = demo_profile
                ? (demo_selector.quality[0].signal_valid
                       ? fmaxf(demo_selector.quality[0].score, 0.01f) : 0.0f)
                : metrics[0].rms;
            const float mic2_weight = demo_profile
                ? (demo_selector.quality[1].signal_valid
                       ? fmaxf(demo_selector.quality[1].score, 0.01f) : 0.0f)
                : metrics[1].rms;
            chord_detector_analyze(
                s_audio_ring[0], s_audio_ring[1], s_ring_write_position,
                mic1_weight, mic2_weight, demo_profile, selected_snr_db,
                low_yin.midi, low_yin.confidence, &low_match,
                &chord);
            counters->chord_time_us = (uint32_t)(esp_timer_get_time() - started);
            harmonic_ratio = yin.valid ?
                chord_detector_harmonic_explained_ratio(yin.frequency_hz) : 0.0f;
#endif
            if (low_match.strongest_midi >= 0 &&
                block_timestamp_ms - last_low_match_log_ms >= 500U) {
                char strongest_name[8] = "?";
                note_midi_to_name(low_match.strongest_midi, strongest_name,
                                  sizeof(strongest_name));
                ESP_LOGI("LOW_MATCH",
                         "best=%s score=%.2f conf=%.2f active=%u signal=%s attack=%s low_yin=%s(%.2f)",
                         strongest_name,
                         (double)low_match.strongest_score,
                         (double)low_match.strongest_confidence,
                         (unsigned)low_match.count,
                         exact_signal_active ? "yes" : "no",
                         exact_attack_allowed ? "yes" : "hold",
                         low_yin.valid ? low_yin.note_name : "-",
                         (double)low_yin.confidence);
                last_low_match_log_ms = block_timestamp_ms;
            }
            /* The exact-key tracker consumes the low-note chain plus the
             * chord evidence layer. Its note set events are additive and do
             * not touch the legacy result stream below. */
            piano_note_set_t note_set;
            bool note_set_changed = piano_note_tracker_update(
                    &s_note_tracker, &chord, &yin, &low_yin, &low_match,
                    harmonic_ratio, exact_signal_active,
                    exact_attack_allowed,
#if MUSIC_USE_SINGLE_MIC_CH1
                    false,
#else
                    demo_profile &&
                        demo_selection.health != DUAL_MIC_HEALTH_DUAL_OK,
#endif
                    exact_rms, block_timestamp_ms,
                    &note_set);
            if (suppressed_low_midi >= 0) {
                if (s_bass_pending_midi == suppressed_low_midi) {
                    s_bass_pending_midi = -1;
                    s_bass_pending_frames = 0;
                }
                if (s_bass_midi == suppressed_low_midi) {
                    s_bass_midi = -1;
                    s_bass_missing_frames = 0;
                }
            }
            /* Bass fallback (C2..B3) with hysteresis. The low-rate YIN is
             * the stable sustained-bass evidence; the Goertzel matcher opens
             * attacks when YIN confidence lags. Requires 2 clean frames to
             * start, tolerates 3 missing frames before release. */
            int bass_candidate = -1;
            const bool bass_attack = exact_signal_active &&
                exact_attack_allowed &&
                bass_attack_supported(&low_match, &low_yin, &bass_candidate);
            if (bass_attack) {
                if (bass_candidate == s_bass_pending_midi) {
                    if (s_bass_pending_frames < UINT8_MAX) {
                        ++s_bass_pending_frames;
                    }
                } else {
                    s_bass_pending_midi = bass_candidate;
                    s_bass_pending_frames = 1;
                }
            } else {
                s_bass_pending_midi = -1;
                s_bass_pending_frames = 0;
            }
            if (s_bass_pending_frames >= 2U &&
                s_bass_midi != s_bass_pending_midi) {
                s_bass_midi = s_bass_pending_midi;
                s_bass_missing_frames = 0;
            }
            if (s_bass_midi >= 0) {
                if (bass_hold_supported(&low_match, &low_yin, s_bass_midi)) {
                    s_bass_missing_frames = 0;
                } else if (++s_bass_missing_frames >= 3U) {
                    s_bass_midi = -1;
                    s_bass_missing_frames = 0;
                }
            }
            if (s_bass_midi >= 0 && note_set.count < PIANO_NOTE_SET_MAX_KEYS) {
                bool already_present = false;
                for (uint8_t index = 0; index < note_set.count; ++index) {
                    if (note_set.midi[index] == s_bass_midi) {
                        already_present = true;
                        break;
                    }
                }
                if (!already_present) {
                    const uint8_t index = note_set.count++;
                    note_set.midi[index] = s_bass_midi;
                    const float bass_confidence =
                        (low_yin.valid && low_yin.midi == s_bass_midi)
                            ? low_yin.confidence
                            : (low_match.valid && low_match.count == 1U &&
                               low_match.midi[0] == s_bass_midi
                                   ? low_match.confidence[0] : 0.85f);
                    note_set.confidence[index] = fminf(
                        fmaxf(bass_confidence, 0.0f), 1.0f);
                    note_set.velocity[index] =
                        low_fallback_velocity(exact_rms);
                    note_set.timestamp_ms = block_timestamp_ms;
                }
            }
            bool bass_present = false;
            for (uint8_t index = 0; index < note_set.count; ++index) {
                if (note_set.midi[index] == s_bass_midi) {
                    bass_present = true;
                    break;
                }
            }
            if (s_bass_injected != bass_present) {
                s_bass_injected = bass_present;
                note_set_changed = true;
            }
            if (note_set_changed) {
                log_exact_note_set(&note_set);
                music_uart_link_submit_note_set(&note_set);
            }
            if (chord.debug_candidate_count > 0) {
                last_spectrum_debug = chord;
                last_spectrum_debug_ms = block_timestamp_ms;
            }
            music_result_t result;
            const char *unknown_reason = "not_analyzed";
            const int poly_stable_votes =
                demo_profile && selected_snr_db >= MUSIC_DEMO_SNR_HIGH_DB
                    ? MUSIC_DEMO_HIGH_SNR_INTERVAL_CONSECUTIVE
                    : (demo_profile && selected_snr_db >= MUSIC_DEMO_SNR_MEDIUM_DB
                           ? MUSIC_DEMO_MEDIUM_SNR_INTERVAL_CONSECUTIVE
                           : MUSIC_STABLE_VOTE_COUNT);
#if MUSIC_USE_SINGLE_MIC_CH1
            const bool emit = music_classifier_update(&classifier, &metrics[0], &metrics[1],
                                                       analysis_active ? 0.0f : preprocess.noise_gate, 0.0f,
                                                       1, &yin, harmonic_ratio, &chord,
                                                       demo_profile, poly_stable_votes,
                                                       block_timestamp_ms,
                                                       &result, &unknown_reason);
#else
            const bool emit = music_classifier_update(&classifier, &metrics[0], &metrics[1],
                                                       preprocess[0].noise_gate, preprocess[1].noise_gate,
                                                       selected_mic, &yin, harmonic_ratio, &chord,
                                                       demo_profile, poly_stable_votes,
                                                       block_timestamp_ms,
                                                       &result, &unknown_reason);
#endif
            const bool tonal_content =
                (yin.valid && yin.confidence >= 0.45f) ||
                chord.debug_candidate_count >= 2;
            if (demo_profile && !tonal_content) {
#if MUSIC_USE_SINGLE_MIC_CH1
                audio_preprocess_track_ambient(&preprocess, &metrics[0]);
#else
                for (int mic = 0; mic < 2; ++mic) {
                    audio_preprocess_track_ambient(&preprocess[mic],
                                                   &metrics[mic]);
                }
#endif
            }
            if (emit) music_uart_link_submit_result(&result);
            if (yin.valid) music_uart_link_submit_pitch(&result);
            if (emit) log_result(&result, unknown_reason, &chord);
        }

        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (calibrated && now_ms - last_diagnostic_ms >= MUSIC_DIAGNOSTIC_INTERVAL_MS) {
            last_diagnostic_ms = now_ms;
#if MUSIC_USE_SINGLE_MIC_CH1
            diagnostics_log_audio(metrics[0].rms, metrics[0].peak, metrics[0].clip_rate,
                                  0.0f, 0.0f, 0.0f, 1, preprocess.noise_floor, 0.0f,
                                  preprocess.noise_gate, 0.0f);
            ESP_LOGI("DSP_DIAG", "yin_conf=%.2f poly_conf=%.2f",
                     last_yin_confidence, last_chord_confidence);
            ESP_LOGI("ADAPTIVE_INPUT",
                     "profile=%s gain=%.0fdB selected=1 snr=%.1fdB tier=%s",
                     demo_profile ? "demo" : "strict",
                     (double)input_control.current_gain_db,
                     (double)selected_snr_db,
                     adaptive_snr_tier(demo_profile, selected_snr_db));
            if (last_spectrum_debug_ms != 0 &&
                now_ms - last_spectrum_debug_ms <= 1000) {
                log_spectrum_debug(&last_spectrum_debug);
            }
            if (metrics[0].clipped) {
#else
            diagnostics_log_audio(metrics[0].rms, metrics[0].peak, metrics[0].clip_rate,
                                  metrics[1].rms, metrics[1].peak, metrics[1].clip_rate,
                                  selected_mic, preprocess[0].noise_floor, preprocess[1].noise_floor,
                                  preprocess[0].noise_gate, preprocess[1].noise_gate);
            ESP_LOGI("ADAPTIVE_INPUT",
                     "profile=%s gain=%.0fdB selected=%d health=%s mic=[%s/%.1fdB,%s/%.1fdB] tier=%s",
                     demo_profile ? "demo" : "strict",
                     (double)input_control.current_gain_db, selected_mic,
                     demo_profile ? dual_mic_health_name(demo_selection.health)
                                  : "strict",
                     demo_profile ? dual_mic_channel_state_name(
                                        demo_selector.quality[0].state)
                                  : "strict",
                     (double)(demo_profile ? demo_selector.quality[0].snr_db
                                          : signal_snr_db(&metrics[0], &preprocess[0])),
                     demo_profile ? dual_mic_channel_state_name(
                                        demo_selector.quality[1].state)
                                  : "strict",
                     (double)(demo_profile ? demo_selector.quality[1].snr_db
                                          : signal_snr_db(&metrics[1], &preprocess[1])),
                     adaptive_snr_tier(demo_profile, selected_snr_db));
            if (last_spectrum_debug_ms != 0 &&
                now_ms - last_spectrum_debug_ms <= 1000) {
                log_spectrum_debug(&last_spectrum_debug);
            }
            if (metrics[0].clipped || metrics[1].clipped) {
#endif
                ESP_LOGW("AUDIO_DIAG", "clipping: reduce ES7210 input gain or increase source distance");
                low_peak_diagnostics = 0;
#if MUSIC_USE_SINGLE_MIC_CH1
            } else if (metrics[0].peak < MUSIC_LOW_PEAK_GAIN_HINT) {
#else
            } else if (fmaxf(metrics[0].peak, metrics[1].peak) < MUSIC_LOW_PEAK_GAIN_HINT) {
#endif
                if (++low_peak_diagnostics >= 10) {
                    ESP_LOGW("AUDIO_DIAG", "peak stayed below 5%% full scale; consider the next higher gain option");
                    low_peak_diagnostics = 0;
                }
            } else {
                low_peak_diagnostics = 0;
            }
        }
        counters->dsp_cycle_time_us = (uint32_t)(esp_timer_get_time() - cycle_start_us);
        const uint32_t block_budget_us = (uint32_t)(1000000ULL * MUSIC_CAPTURE_FRAMES / MUSIC_SAMPLE_RATE_HZ);
        if (counters->dsp_cycle_time_us > block_budget_us) ++counters->dsp_deadline_miss_count;
        if (now_ms - last_performance_ms >= MUSIC_PERFORMANCE_INTERVAL_MS) {
            last_performance_ms = now_ms;
            diagnostics_log_performance(es7210_capture_queue_depth(),
                                        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                                        uxTaskGetStackHighWaterMark(NULL));
        }
    }
}

int music_detector_start(void)
{
    ESP_LOGI("MUSIC", "recognition profile: strict");
    ESP_RETURN_ON_ERROR(es7210_capture_init(), "MUSIC", "audio hardware initialization failed");
    ESP_RETURN_ON_FALSE(chord_detector_init() == ESP_OK, ESP_FAIL, "MUSIC", "ESP-DSP FFT init failed");
    ESP_RETURN_ON_ERROR(es7210_capture_start(), "MUSIC", "capture task creation failed");
    const BaseType_t task_result = xTaskCreatePinnedToCore(music_dsp_task, "MusicDspTask",
                                                           MUSIC_DSP_TASK_STACK_SIZE,
                                                           NULL, MUSIC_DSP_TASK_PRIORITY,
                                                           NULL, MUSIC_DSP_TASK_CORE);
    ESP_RETURN_ON_FALSE(task_result == pdPASS, ESP_ERR_NO_MEM, "MUSIC", "DSP task creation failed");
    return ESP_OK;
}
