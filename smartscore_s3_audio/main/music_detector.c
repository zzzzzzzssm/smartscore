#include "music_detector.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "audio_preprocess.h"
#include "chord_detector.h"
#include "diagnostics.h"
#include "es7210_capture.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "music_classifier.h"
#include "music_detector_config.h"
#include "music_uart_link.h"
#include "note_utils.h"
#include "yin_detector.h"

#if MUSIC_USE_SINGLE_MIC_CH1
static float s_audio_ring[MUSIC_FFT_SIZE];
static float s_frame_float[MUSIC_CAPTURE_FRAMES];
#else
static float s_audio_ring[2][MUSIC_FFT_SIZE];
static float s_frame_float[2][MUSIC_CAPTURE_FRAMES];
#endif
static float s_yin_window[MUSIC_YIN_WINDOW_SIZE];
static size_t s_ring_write_position;
static size_t s_ring_filled;

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
    ESP_LOGI("SPECTRUM_DEBUG", "active=%d noise=%.6f candidates=%d harmonic_rejected=%d top=[%s;%s;%s]",
             spectrum->independent_pitch_class_count, spectrum->spectrum_noise_floor,
             spectrum->debug_candidate_count, spectrum->harmonic_rejected_count,
             entries[0], entries[1], entries[2]);
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
    yin_detector_init();
    int selected_mic = 1;
#if !MUSIC_USE_SINGLE_MIC_CH1
    int challenger_count = 0;
#endif
    uint32_t calibration_start_ms = 0;
    uint32_t last_diagnostic_ms = 0;
    uint32_t last_performance_ms = 0;
    uint32_t last_spectrum_debug_ms = 0;
    uint32_t low_peak_diagnostics = 0;
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
#if MUSIC_USE_SINGLE_MIC_CH1
        audio_preprocess_frame(&preprocess, block->mic1, s_frame_float,
                               MUSIC_CAPTURE_FRAMES, &metrics[0]);
        for (size_t i = 0; i < MUSIC_CAPTURE_FRAMES; ++i) {
            const size_t index = (s_ring_write_position + i) % MUSIC_FFT_SIZE;
            s_audio_ring[index] = s_frame_float[i];
        }
#else
        audio_preprocess_frame(&preprocess[0], block->mic1, s_frame_float[0],
                               MUSIC_CAPTURE_FRAMES, &metrics[0]);
        audio_preprocess_frame(&preprocess[1], block->mic2, s_frame_float[1],
                               MUSIC_CAPTURE_FRAMES, &metrics[1]);
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

        if (calibrated && s_ring_filled >= MUSIC_FFT_SIZE) {
            yin_result_t yin = {.midi = -1};
            chord_result_t chord = {0};
            float harmonic_ratio = 0.0f;
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
                int64_t started = esp_timer_get_time();
                yin_detector_analyze(s_yin_window, MUSIC_YIN_WINDOW_SIZE, &yin);
                counters->yin_time_us = (uint32_t)(esp_timer_get_time() - started);
                started = esp_timer_get_time();
                chord_detector_analyze(s_audio_ring, NULL, s_ring_write_position,
                                       metrics[0].rms, 0.0f, &chord);
                counters->chord_time_us = (uint32_t)(esp_timer_get_time() - started);
                harmonic_ratio = yin.valid ?
                    chord_detector_harmonic_explained_ratio(yin.frequency_hz) : 0.0f;
            } else {
                counters->yin_time_us = 0;
                counters->mic1_fft_time_us = 0;
                counters->mic2_fft_time_us = 0;
                counters->chord_time_us = 0;
            }
            last_yin_confidence = yin.confidence;
            last_chord_confidence = chord.confidence;
#else
            selected_mic = update_selected_mic(selected_mic, &challenger_count, metrics, preprocess);
            copy_yin_window(selected_mic);
            int64_t started = esp_timer_get_time();
            yin_detector_analyze(s_yin_window, MUSIC_YIN_WINDOW_SIZE, &yin);
            counters->yin_time_us = (uint32_t)(esp_timer_get_time() - started);
            started = esp_timer_get_time();
            chord_detector_analyze(s_audio_ring[0], s_audio_ring[1], s_ring_write_position,
                                   metrics[0].rms, metrics[1].rms, &chord);
            counters->chord_time_us = (uint32_t)(esp_timer_get_time() - started);
            harmonic_ratio = yin.valid ?
                chord_detector_harmonic_explained_ratio(yin.frequency_hz) : 0.0f;
#endif
            if (chord.debug_candidate_count > 0) {
                last_spectrum_debug = chord;
                last_spectrum_debug_ms = block_timestamp_ms;
            }
            music_result_t result;
            const char *unknown_reason = "not_analyzed";
#if MUSIC_USE_SINGLE_MIC_CH1
            const bool emit = music_classifier_update(&classifier, &metrics[0], &metrics[1],
                                                       analysis_active ? 0.0f : preprocess.noise_gate, 0.0f,
                                                       1, &yin, harmonic_ratio, &chord,
                                                       block_timestamp_ms, &result, &unknown_reason);
#else
            const bool emit = music_classifier_update(&classifier, &metrics[0], &metrics[1],
                                                       preprocess[0].noise_gate, preprocess[1].noise_gate,
                                                       selected_mic, &yin, harmonic_ratio, &chord,
                                                       block_timestamp_ms, &result, &unknown_reason);
#endif
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
    ESP_RETURN_ON_ERROR(es7210_capture_init(), "MUSIC", "audio hardware initialization failed");
    ESP_RETURN_ON_FALSE(chord_detector_init() == ESP_OK, ESP_FAIL, "MUSIC", "ESP-DSP FFT init failed");
    ESP_RETURN_ON_ERROR(es7210_capture_start(), "MUSIC", "capture task creation failed");
    const BaseType_t task_result = xTaskCreatePinnedToCore(music_dsp_task, "MusicDspTask", 6144,
                                                           NULL, 12, NULL, 1);
    ESP_RETURN_ON_FALSE(task_result == pdPASS, ESP_ERR_NO_MEM, "MUSIC", "DSP task creation failed");
    return ESP_OK;
}
