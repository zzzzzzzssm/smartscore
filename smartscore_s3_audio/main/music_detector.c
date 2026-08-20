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
#include "esp_psram.h"
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

typedef struct {
#if MUSIC_USE_SINGLE_MIC_CH1
    float audio_ring[MUSIC_FFT_SIZE];
    float frame_float[MUSIC_CAPTURE_FRAMES];
    audio_preprocess_state_t preprocess;
#else
    float audio_ring[2][MUSIC_FFT_SIZE];
    float frame_float[2][MUSIC_CAPTURE_FRAMES];
    audio_preprocess_state_t preprocess[2];
    dual_mic_selector_t demo_selector;
    dual_mic_selection_t demo_selection;
#endif
    float yin_window[MUSIC_YIN_WINDOW_SIZE];
    float low_yin_window[MUSIC_LOW_YIN_WINDOW_SIZE];
    low_note_detector_t low_note_detector;
    low_yin_stabilizer_t low_yin_stabilizer;
    audio_frame_metrics_t metrics[2];
    music_classifier_t classifier;
    adaptive_input_control_t input_control;
    piano_note_tracker_t note_tracker;
    chord_result_t last_spectrum_debug;
} music_dsp_context_t;

static music_dsp_context_t *s_dsp_context;
static size_t s_ring_write_position;
static size_t s_ring_filled;
static volatile music_recognition_profile_t s_recognition_profile =
    MUSIC_RECOGNITION_PROFILE_STRICT;

#define s_audio_ring (s_dsp_context->audio_ring)
#define s_frame_float (s_dsp_context->frame_float)
#define s_yin_window (s_dsp_context->yin_window)
#define s_low_yin_window (s_dsp_context->low_yin_window)
#define s_low_note_detector (s_dsp_context->low_note_detector)

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

void music_detector_set_recognition_profile(music_recognition_profile_t profile)
{
    if (profile != MUSIC_RECOGNITION_PROFILE_DEMO &&
        profile != MUSIC_RECOGNITION_PROFILE_PERFORMANCE) {
        profile = MUSIC_RECOGNITION_PROFILE_STRICT;
    }
    const music_recognition_profile_t previous = s_recognition_profile;
    s_recognition_profile = profile;
    if (previous != profile) {
        const char *name = profile == MUSIC_RECOGNITION_PROFILE_PERFORMANCE
                               ? "performance"
                               : (profile == MUSIC_RECOGNITION_PROFILE_DEMO
                                      ? "demo" : "strict");
        ESP_LOGI("MUSIC", "recognition profile: %s", name);
    }
}

music_recognition_profile_t music_detector_get_recognition_profile(void)
{
    const music_recognition_profile_t profile = s_recognition_profile;
    return profile == MUSIC_RECOGNITION_PROFILE_DEMO ||
                   profile == MUSIC_RECOGNITION_PROFILE_PERFORMANCE
               ? profile : MUSIC_RECOGNITION_PROFILE_STRICT;
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
    low_note_detector_reset(&s_low_note_detector);
    low_yin_stabilizer_reset(&s_dsp_context->low_yin_stabilizer);
}

static bool copy_low_yin_window(int selected_mic)
{
    return low_note_detector_copy_recent(
        &s_low_note_detector, selected_mic - 1, s_low_yin_window,
        MUSIC_LOW_YIN_WINDOW_SIZE);
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

#define preprocess (context->preprocess)
#define metrics (context->metrics)
#define classifier (context->classifier)
#define input_control (context->input_control)
#define note_tracker (context->note_tracker)
#define low_note_detector (context->low_note_detector)
#define low_yin_stabilizer (context->low_yin_stabilizer)
#if !MUSIC_USE_SINGLE_MIC_CH1
#define demo_selector (context->demo_selector)
#define demo_selection (context->demo_selection)
#endif
#define last_spectrum_debug (context->last_spectrum_debug)

static void music_dsp_task(void *argument)
{
    music_dsp_context_t *context = (music_dsp_context_t *)argument;
    if (context == NULL) {
        ESP_LOGE("MUSIC", "DSP task received a null PSRAM context");
        vTaskDelete(NULL);
        return;
    }
#if MUSIC_USE_SINGLE_MIC_CH1
    audio_preprocess_init(&preprocess);
#else
    for (int mic = 0; mic < 2; ++mic) audio_preprocess_init(&preprocess[mic]);
#endif
    music_classifier_init(&classifier);
    adaptive_input_control_init(&input_control,
                                es7210_capture_get_input_gain());
    music_recognition_profile_t active_profile =
        MUSIC_RECOGNITION_PROFILE_STRICT;
    bool active_profile_initialized = false;
    yin_detector_init();
    low_note_detector_init(&low_note_detector);
    low_yin_stabilizer_reset(&low_yin_stabilizer);
    int selected_mic = 1;
    piano_note_tracker_init(&note_tracker);
    unsigned spectrum_hop_counter = 0;
#if !MUSIC_USE_SINGLE_MIC_CH1
    dual_mic_selector_init(&demo_selector);
#endif
    uint32_t calibration_start_ms = 0;
    uint32_t last_diagnostic_ms = 0;
    uint32_t last_performance_ms = 0;
    uint32_t last_spectrum_debug_ms = 0;
    uint32_t last_low_match_log_ms = 0;
    uint32_t low_peak_diagnostics = 0;
    bool low_dsp_stack_warned = false;
    float selected_snr_db = -120.0f;
#if MUSIC_USE_SINGLE_MIC_CH1
    float last_yin_confidence = 0.0f;
    float last_chord_confidence = 0.0f;
#endif
    unsigned active_hangover_blocks = 0;
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
                        &note_tracker, block_timestamp_ms, &released_set)) {
                    log_exact_note_set(&released_set);
                    music_uart_link_submit_note_set(&released_set);
                }
            } else {
                piano_note_tracker_init(&note_tracker);
            }
            active_profile_initialized = true;
            active_profile = requested_profile;
            music_classifier_init(&classifier);
            low_note_detector_reset(&low_note_detector);
            low_yin_stabilizer_reset(&low_yin_stabilizer);
            spectrum_hop_counter = 0;
#if !MUSIC_USE_SINGLE_MIC_CH1
            dual_mic_selector_init(&demo_selector);
#endif
            const char *profile_name =
                active_profile == MUSIC_RECOGNITION_PROFILE_PERFORMANCE
                    ? "performance"
                    : (active_profile == MUSIC_RECOGNITION_PROFILE_DEMO
                           ? "demo" : "strict");
            ESP_LOGI("MUSIC", "recognition history reset for profile: %s",
                     profile_name);
            float requested_gain_db = input_control.current_gain_db;
            adaptive_gain_reason_t gain_reason = ADAPTIVE_GAIN_REASON_NONE;
            if (adaptive_input_control_profile_target(
                    &input_control,
                    active_profile != MUSIC_RECOGNITION_PROFILE_STRICT,
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
            active_profile != MUSIC_RECOGNITION_PROFILE_STRICT;
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
        dual_mic_selector_update(&demo_selector, metrics, preprocess,
                                 &demo_selection);
        if (demo_selection.switched) {
            low_yin_stabilizer_reset(&low_yin_stabilizer);
        }
        selected_mic = demo_selection.selected_mic;
        selected_snr_db = demo_selector.quality[selected_mic - 1].snr_db;
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
        low_note_detector_push(&low_note_detector, 0, s_frame_float,
                               MUSIC_CAPTURE_FRAMES);
        for (size_t i = 0; i < MUSIC_CAPTURE_FRAMES; ++i) {
            const size_t index = (s_ring_write_position + i) % MUSIC_FFT_SIZE;
            s_audio_ring[index] = s_frame_float[i];
        }
#else
        low_note_detector_push(&low_note_detector, 0, s_frame_float[0],
                               MUSIC_CAPTURE_FRAMES);
        low_note_detector_push(&low_note_detector, 1, s_frame_float[1],
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

        const uint32_t calibration_elapsed_ms =
            block_timestamp_ms - calibration_start_ms;
#if MUSIC_USE_SINGLE_MIC_CH1
        const bool calibration_quiet_ready =
            audio_preprocess_calibration_ready(&preprocess);
#else
        const bool calibration_quiet_ready =
            audio_preprocess_calibration_ready(&preprocess[0]) &&
            audio_preprocess_calibration_ready(&preprocess[1]);
#endif
        const bool calibration_timed_out =
            calibration_elapsed_ms >= MUSIC_CALIBRATION_MAX_MS;
        if (!calibrated &&
            ((calibration_elapsed_ms >= MUSIC_CALIBRATION_MS &&
              calibration_quiet_ready) || calibration_timed_out)) {
#if MUSIC_USE_SINGLE_MIC_CH1
            audio_preprocess_finish_calibration(&preprocess);
            calibrated = true;
            ESP_LOGI("AUDIO",
                     "CH1 calibration finished status=%s valid=%" PRIu32
                     " rejected=%" PRIu32,
                     calibration_quiet_ready ? "quiet" : "timeout",
                     preprocess.calibration_frames,
                     preprocess.calibration_rejected_frames);
            ESP_LOGI("AUDIO", "ch1_noise_rms=%.5f ch1_noise_gate=%.5f",
                     preprocess.noise_floor, preprocess.noise_gate);
#else
            audio_preprocess_finish_calibration(&preprocess[0]);
            audio_preprocess_finish_calibration(&preprocess[1]);
            calibrated = true;
            ESP_LOGI("AUDIO",
                     "calibration finished status=%s mic1_valid=%" PRIu32
                     " mic1_rejected=%" PRIu32 " mic2_valid=%" PRIu32
                     " mic2_rejected=%" PRIu32,
                     calibration_quiet_ready ? "quiet" : "timeout",
                     preprocess[0].calibration_frames,
                     preprocess[0].calibration_rejected_frames,
                     preprocess[1].calibration_frames,
                     preprocess[1].calibration_rejected_frames);
            ESP_LOGI("AUDIO", "mic1_noise=%.5f mic2_noise=%.5f mic1_gate=%.5f mic2_gate=%.5f",
                     preprocess[0].noise_floor, preprocess[1].noise_floor,
                     preprocess[0].noise_gate, preprocess[1].noise_gate);
#endif
        }

        if (calibrated && s_ring_filled >= MUSIC_FFT_SIZE &&
            (++spectrum_hop_counter % MUSIC_SPECTRUM_ANALYSIS_HOPS) == 0) {
            yin_result_t yin = {.midi = -1};
            yin_result_t high_yin = {.midi = -1};
            yin_result_t raw_low_yin = {.midi = -1};
            yin_result_t low_yin = {.midi = -1};
            low_note_result_t low_match = {0};
            chord_result_t chord = {0};
            float harmonic_ratio = 0.0f;
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
                yin_detector_analyze_range(
                    s_yin_window, MUSIC_YIN_WINDOW_SIZE, MUSIC_SAMPLE_RATE_HZ,
                    MUSIC_HIGH_YIN_MIN_FREQUENCY_HZ,
                    MUSIC_MAX_FREQUENCY_HZ, &high_yin);
                if (low_window_ready) {
                    yin_detector_analyze_range(
                        s_low_yin_window, MUSIC_LOW_YIN_WINDOW_SIZE,
                        MUSIC_LOW_YIN_SAMPLE_RATE_HZ, MUSIC_MIN_FREQUENCY_HZ,
                        MUSIC_LOW_YIN_MAX_FREQUENCY_HZ, &raw_low_yin);
                }
                low_yin_stabilizer_update(
                    &low_yin_stabilizer, &raw_low_yin, true, &low_yin);
                exact_signal_active = true;
                exact_attack_allowed = above_gate && !metrics[0].clipped;
                exact_rms = metrics[0].rms;
                low_note_detector_analyze(
                    &low_note_detector, 0, true, exact_attack_allowed,
                    metrics[0].clipped,
                    &low_match);
                if (low_match.tonal_override) {
                    exact_attack_allowed = true;
                }
                promote_low_match_yin(&low_match, &low_yin);
                counters->yin_time_us = (uint32_t)(esp_timer_get_time() - started);
                started = esp_timer_get_time();
                chord_detector_analyze(s_audio_ring, NULL, s_ring_write_position,
                                       metrics[0].rms, 0.0f, demo_profile,
                                       selected_snr_db, low_yin.midi,
                                       low_yin.confidence, &low_match,
                                       &chord);
                counters->chord_time_us = (uint32_t)(esp_timer_get_time() - started);
            } else {
                low_yin_stabilizer_update(
                    &low_yin_stabilizer, NULL, false, &low_yin);
                low_note_detector_analyze(
                    &low_note_detector, 0, false, false,
                    metrics[0].clipped, &low_match);
                if (low_match.tonal_override) {
                    exact_signal_active = true;
                    exact_attack_allowed = true;
                    exact_rms = metrics[0].rms;
                    promote_low_match_yin(&low_match, &low_yin);
                    const int64_t started = esp_timer_get_time();
                    chord_detector_analyze(
                        s_audio_ring, NULL, s_ring_write_position,
                        metrics[0].rms, 0.0f, demo_profile,
                        selected_snr_db, low_yin.midi,
                        low_yin.confidence, &low_match, &chord);
                    counters->chord_time_us = (uint32_t)(
                        esp_timer_get_time() - started);
                }
                counters->yin_time_us = 0;
                if (!low_match.tonal_override) {
                    counters->mic1_fft_time_us = 0;
                    counters->mic2_fft_time_us = 0;
                    counters->chord_time_us = 0;
                }
            }
            last_chord_confidence = chord.confidence;
#else
            const bool exact_signal_observed =
                demo_selector.quality[selected_mic - 1].signal_valid &&
                selected_snr_db >= MUSIC_MIC_MIN_SNR_DB;
            if (exact_signal_observed) {
                active_hangover_blocks = MUSIC_ACTIVITY_HANGOVER_BLOCKS;
            } else if (active_hangover_blocks > 0U) {
                --active_hangover_blocks;
            }
            exact_signal_active = exact_signal_observed ||
                                  active_hangover_blocks > 0U;
            exact_attack_allowed = exact_signal_observed &&
                !metrics[selected_mic - 1].clipped;
            exact_rms = metrics[selected_mic - 1].rms;
            int64_t started = esp_timer_get_time();
            if (exact_signal_active) {
                copy_yin_window(selected_mic);
                const bool low_window_ready =
                    copy_low_yin_window(selected_mic);
                yin_detector_analyze_range(
                    s_yin_window, MUSIC_YIN_WINDOW_SIZE, MUSIC_SAMPLE_RATE_HZ,
                    MUSIC_HIGH_YIN_MIN_FREQUENCY_HZ,
                    MUSIC_MAX_FREQUENCY_HZ, &high_yin);
                if (low_window_ready) {
                    yin_detector_analyze_range(
                        s_low_yin_window, MUSIC_LOW_YIN_WINDOW_SIZE,
                        MUSIC_LOW_YIN_SAMPLE_RATE_HZ, MUSIC_MIN_FREQUENCY_HZ,
                        MUSIC_LOW_YIN_MAX_FREQUENCY_HZ, &raw_low_yin);
                }
                low_yin_stabilizer_update(
                    &low_yin_stabilizer, &raw_low_yin, true, &low_yin);
            } else {
                low_yin_stabilizer_update(
                    &low_yin_stabilizer, NULL, false, &low_yin);
            }
            low_note_detector_analyze(
                &low_note_detector, selected_mic - 1,
                exact_signal_active, exact_attack_allowed,
                metrics[selected_mic - 1].clipped, &low_match);
            if (low_match.tonal_override) {
                exact_signal_active = true;
                exact_attack_allowed = true;
            }
            promote_low_match_yin(&low_match, &low_yin);
            counters->yin_time_us = (uint32_t)(esp_timer_get_time() - started);
            started = esp_timer_get_time();
            const float mic1_weight = demo_selector.quality[0].signal_valid
                ? fmaxf(demo_selector.quality[0].score, 0.01f) : 0.0f;
            const float mic2_weight = demo_selector.quality[1].signal_valid
                ? fmaxf(demo_selector.quality[1].score, 0.01f) : 0.0f;
            if (exact_signal_active) {
                chord_detector_analyze(
                    s_audio_ring[0], s_audio_ring[1], s_ring_write_position,
                    mic1_weight, mic2_weight, true, selected_snr_db,
                    low_yin.midi, low_yin.confidence, &low_match, &chord);
            }
            counters->chord_time_us = (uint32_t)(esp_timer_get_time() - started);
#endif
            if (low_yin.valid &&
                low_yin.midi <= MUSIC_VIRTUAL_FUNDAMENTAL_MAX_MIDI &&
                (!high_yin.valid || low_yin.confidence >= high_yin.confidence)) {
                yin = low_yin;
            } else {
                yin = high_yin;
            }
            if (low_match.strongest_midi >= 0 &&
                block_timestamp_ms - last_low_match_log_ms >= 500U) {
                char strongest_name[8] = "?";
                note_midi_to_name(low_match.strongest_midi, strongest_name,
                                  sizeof(strongest_name));
                ESP_LOGI("LOW_MATCH",
                         "best=%s score=%.2f conf=%.2f active=%u signal=%s attack=%s",
                         strongest_name,
                         (double)low_match.strongest_score,
                         (double)low_match.strongest_confidence,
                         (unsigned)low_match.count,
                         exact_signal_active ? "yes" : "no",
                         exact_attack_allowed ? "yes" : "hold");
                last_low_match_log_ms = block_timestamp_ms;
            }
#if MUSIC_USE_SINGLE_MIC_CH1
            last_yin_confidence = yin.confidence;
#endif
            harmonic_ratio = yin.valid ?
                chord_detector_harmonic_explained_ratio(yin.frequency_hz) : 0.0f;
            piano_note_set_t note_set;
            if (piano_note_tracker_update(
                    &note_tracker, &chord, &high_yin, &low_yin,
                    &low_match,
                    harmonic_ratio, exact_signal_active,
                    exact_attack_allowed,
#if MUSIC_USE_SINGLE_MIC_CH1
                    false,
#else
                    demo_selection.health != DUAL_MIC_HEALTH_DUAL_OK,
#endif
                    exact_rms, block_timestamp_ms,
                    &note_set)) {
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
                    ? MUSIC_DEMO_HIGH_SNR_STABLE_VOTES
                    : MUSIC_STABLE_VOTE_COUNT;
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
            const UBaseType_t stack_bytes = uxTaskGetStackHighWaterMark(NULL);
            if (!low_dsp_stack_warned &&
                stack_bytes < MUSIC_DSP_STACK_WARN_BYTES) {
                low_dsp_stack_warned = true;
                ESP_LOGE("MUSIC", "MusicDspTask low stack: %u bytes remaining",
                         (unsigned)stack_bytes);
            }
            diagnostics_log_performance(es7210_capture_queue_depth(),
                                        heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                                        stack_bytes);
        }
    }
}

#undef preprocess
#undef metrics
#undef classifier
#undef input_control
#undef note_tracker
#undef low_note_detector
#undef low_yin_stabilizer
#if !MUSIC_USE_SINGLE_MIC_CH1
#undef demo_selector
#undef demo_selection
#endif
#undef last_spectrum_debug

int music_detector_start(void)
{
    ESP_LOGI("MUSIC", "recognition profile: strict");
    ESP_RETURN_ON_FALSE(esp_psram_is_initialized(), ESP_ERR_INVALID_STATE,
                        "MUSIC", "N16R8 PSRAM is not initialized");
    ESP_RETURN_ON_FALSE(s_dsp_context == NULL, ESP_ERR_INVALID_STATE,
                        "MUSIC", "DSP detector is already started");
    s_dsp_context = heap_caps_aligned_calloc(
        16, 1, sizeof(*s_dsp_context),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_dsp_context != NULL, ESP_ERR_NO_MEM, "MUSIC",
                        "DSP PSRAM context allocation failed: %u bytes",
                        (unsigned)sizeof(*s_dsp_context));

    esp_err_t error = es7210_capture_init();
    if (error != ESP_OK) {
        heap_caps_free(s_dsp_context);
        s_dsp_context = NULL;
        ESP_RETURN_ON_ERROR(error, "MUSIC", "audio hardware initialization failed");
    }
    error = chord_detector_init();
    if (error != ESP_OK) {
        heap_caps_free(s_dsp_context);
        s_dsp_context = NULL;
        ESP_RETURN_ON_ERROR(error, "MUSIC", "ESP-DSP PSRAM workspace initialization failed");
    }

    TaskHandle_t dsp_task = NULL;
    const BaseType_t task_result = xTaskCreatePinnedToCore(
        music_dsp_task, "MusicDspTask", MUSIC_DSP_TASK_STACK_SIZE,
        s_dsp_context, MUSIC_DSP_TASK_PRIORITY, &dsp_task,
        MUSIC_DSP_TASK_CORE);
    if (task_result != pdPASS) {
        heap_caps_free(s_dsp_context);
        s_dsp_context = NULL;
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, "MUSIC",
                            "DSP task creation failed");
    }

    error = es7210_capture_start();
    if (error != ESP_OK) {
        vTaskDelete(dsp_task);
        heap_caps_free(s_dsp_context);
        s_dsp_context = NULL;
        ESP_RETURN_ON_ERROR(error, "MUSIC", "capture task creation failed");
    }
    ESP_LOGI("MUSIC",
             "PSRAM ready: total=%u free=%u context=%u chord_workspace=%u; DSP stack=%u bytes internal",
             (unsigned)esp_psram_get_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)sizeof(*s_dsp_context),
             (unsigned)chord_detector_workspace_size(),
             (unsigned)MUSIC_DSP_TASK_STACK_SIZE);
    return ESP_OK;
}
