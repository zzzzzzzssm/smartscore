#include "music_detector.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
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
#include "music_classifier.h"
#include "music_detector_config.h"
#include "music_uart_link.h"
#include "note_utils.h"
#include "yin_detector.h"

#if MUSIC_USE_SINGLE_MIC_CH1
static float s_audio_ring[MUSIC_FFT_SIZE];
#else
static float s_audio_ring[2][MUSIC_FFT_SIZE];
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
            ESP_LOGI("RESULT", "SINGLE note=%s midi=%d freq=%.1fHz cents=%+.1f conf=%.2f mic=%d harmonic_ratio=%.2f onset=%s octave_fix=%s",
                     result->note_name, result->midi, result->frequency_hz, result->cents,
                     result->confidence, result->selected_mic,
                     result->harmonic_explained_ratio,
                     result->onset ? "yes" : "no",
                     result->octave_corrected ? "yes" : "no");
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
            ESP_LOGI("RESULT", "INTERVAL notes=[%s,%s] pitch_classes=[%s,%s] midi=[%d,%d] conf=%.2f mic=%d",
#endif
                     first_note, second_note,
                     note_pitch_class_name(result->pitch_classes[0]),
                     note_pitch_class_name(result->pitch_classes[1]),
                     result->midi_notes[0], result->midi_notes[1], result->confidence
#if !MUSIC_USE_SINGLE_MIC_CH1
                     , result->selected_mic
#endif
                     );
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
            ESP_LOGI("RESULT", "CHORD chord=%s notes=[%s,%s,%s] conf=%.2f mic=%d",
#endif
                     result->chord_name, note_pitch_class_name(result->pitch_classes[0]),
                     note_pitch_class_name(result->pitch_classes[1]),
                     note_pitch_class_name(result->pitch_classes[2]), result->confidence
#if !MUSIC_USE_SINGLE_MIC_CH1
                     , result->selected_mic
#endif
                     );
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

static bool result_log_changed(const music_result_t *left,
                               const music_result_t *right)
{
    if (left->type != right->type) return true;
    if (left->type == MUSIC_RESULT_SINGLE) {
        return left->midi != right->midi || left->onset;
    }
    if (left->type == MUSIC_RESULT_INTERVAL) {
        return left->pitch_classes[0] != right->pitch_classes[0] ||
               left->pitch_classes[1] != right->pitch_classes[1];
    }
    if (left->type == MUSIC_RESULT_CHORD) {
        return left->chord_root != right->chord_root ||
               left->chord_is_minor != right->chord_is_minor;
    }
    return false;
}

static void music_dsp_task(void *argument)
{
    (void)argument;
#if MUSIC_USE_SINGLE_MIC_CH1
    audio_preprocess_state_t preprocess;
#else
    audio_preprocess_state_t preprocess[2];
    dual_mic_selector_t mic_selector;
    dual_mic_selection_t mic_selection = {.selected_mic = 1};
#endif
    audio_frame_metrics_t metrics[2] = {0};
    music_classifier_t classifier;
#if MUSIC_USE_SINGLE_MIC_CH1
    audio_preprocess_init(&preprocess);
#else
    for (int mic = 0; mic < 2; ++mic) audio_preprocess_init(&preprocess[mic]);
    dual_mic_selector_init(&mic_selector);
#endif
    music_classifier_init(&classifier);
    yin_detector_init();
    int selected_mic = 1;
    uint32_t calibration_start_ms = 0;
    uint32_t last_diagnostic_ms = 0;
    uint32_t last_performance_ms = 0;
    uint32_t last_spectrum_debug_ms = 0;
    uint32_t low_peak_diagnostics = 0;
    chord_result_t last_spectrum_debug = {0};
    chord_result_t cached_spectrum = {0};
    bool cached_spectrum_valid = false;
    bool analysis_was_active = false;
    unsigned spectrum_hop = 0;
    float last_yin_confidence = 0.0f;
    float last_chord_confidence = 0.0f;
    music_result_t last_logged_result = {0};
    bool have_logged_result = false;
    uint32_t last_result_log_ms = 0;
    unsigned active_hangover_blocks = 0;
#if !MUSIC_USE_SINGLE_MIC_CH1
    bool startup_health_logged = false;
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
        audio_preprocess_frame(&preprocess, block->mic1,
                               &s_audio_ring[s_ring_write_position],
                               MUSIC_CAPTURE_FRAMES, &metrics[0]);
#else
        audio_preprocess_frame(&preprocess[0], block->mic1,
                               &s_audio_ring[0][s_ring_write_position],
                               MUSIC_CAPTURE_FRAMES, &metrics[0]);
        audio_preprocess_frame(&preprocess[1], block->mic2,
                               &s_audio_ring[1][s_ring_write_position],
                               MUSIC_CAPTURE_FRAMES, &metrics[1]);
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
                const bool run_spectrum =
                    !cached_spectrum_valid || !analysis_was_active ||
                    spectrum_hop == 0;
                spectrum_hop = (spectrum_hop + 1U) %
                               MUSIC_SPECTRUM_ANALYSIS_HOPS;
                if (run_spectrum) {
                    started = esp_timer_get_time();
                    chord_detector_analyze(
                        s_audio_ring, NULL, s_ring_write_position,
                        metrics[0].rms, 0.0f, &cached_spectrum);
                    counters->chord_time_us =
                        (uint32_t)(esp_timer_get_time() - started);
                    cached_spectrum_valid = true;
                } else {
                    counters->mic1_fft_time_us = 0;
                    counters->mic2_fft_time_us = 0;
                    counters->chord_time_us = 0;
                }
                chord = cached_spectrum;
                harmonic_ratio = yin.valid ?
                    chord_detector_harmonic_explained_ratio(yin.frequency_hz) : 0.0f;
            } else {
                counters->yin_time_us = 0;
                counters->mic1_fft_time_us = 0;
                counters->mic2_fft_time_us = 0;
                counters->chord_time_us = 0;
                cached_spectrum_valid = false;
                spectrum_hop = 0;
            }
            analysis_was_active = analysis_active;
            last_yin_confidence = yin.confidence;
            last_chord_confidence = chord.confidence;
#else
            dual_mic_selector_update(&mic_selector, metrics, preprocess, &mic_selection);
            selected_mic = mic_selection.selected_mic;
            if (mic_selection.switched) {
                counters->mic_switch_count = mic_selector.switch_count;
                ESP_LOGI("DUAL_MIC", "primary channel switched to MIC%d health=%s switches=%" PRIu32,
                         selected_mic, dual_mic_health_name(mic_selection.health),
                         mic_selector.switch_count);
            }
            const bool analysis_active = mic_selection.sound_active || active_hangover_blocks > 0;
            if (mic_selection.sound_active) {
                active_hangover_blocks = MUSIC_ACTIVITY_HANGOVER_BLOCKS;
            } else if (active_hangover_blocks > 0) {
                --active_hangover_blocks;
            }
            if (analysis_active) {
                copy_yin_window(selected_mic);
                int64_t started = esp_timer_get_time();
                yin_detector_analyze(s_yin_window, MUSIC_YIN_WINDOW_SIZE, &yin);
                counters->yin_time_us = (uint32_t)(esp_timer_get_time() - started);
                const bool run_spectrum =
                    !cached_spectrum_valid || !analysis_was_active ||
                    spectrum_hop == 0;
                spectrum_hop = (spectrum_hop + 1U) %
                               MUSIC_SPECTRUM_ANALYSIS_HOPS;
                if (run_spectrum) {
                    started = esp_timer_get_time();
                    chord_detector_analyze(
                        s_audio_ring[0], s_audio_ring[1],
                        s_ring_write_position, metrics[0].rms,
                        metrics[1].rms, &cached_spectrum);
                    counters->chord_time_us =
                        (uint32_t)(esp_timer_get_time() - started);
                    cached_spectrum_valid = true;
                } else {
                    counters->mic1_fft_time_us = 0;
                    counters->mic2_fft_time_us = 0;
                    counters->chord_time_us = 0;
                }
                chord = cached_spectrum;
                harmonic_ratio = yin.valid ?
                    chord_detector_harmonic_explained_ratio(yin.frequency_hz) : 0.0f;
            } else {
                counters->yin_time_us = 0;
                counters->mic1_fft_time_us = 0;
                counters->mic2_fft_time_us = 0;
                counters->chord_time_us = 0;
                cached_spectrum_valid = false;
                spectrum_hop = 0;
            }
            analysis_was_active = analysis_active;
#endif
            last_yin_confidence = yin.confidence;
            last_chord_confidence = chord.confidence;
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
                                                       analysis_active ? 0.0f : 2.0f,
                                                       analysis_active ? 0.0f : 2.0f,
                                                       selected_mic, &yin, harmonic_ratio, &chord,
                                                       block_timestamp_ms, &result, &unknown_reason);
#endif
            if (emit) music_uart_link_submit_result(&result);
            if (yin.valid) music_uart_link_submit_pitch(&result);
            if (emit &&
                (!have_logged_result ||
                 result_log_changed(&result, &last_logged_result) ||
                 block_timestamp_ms - last_result_log_ms >=
                     MUSIC_RESULT_LOG_REPEAT_INTERVAL_MS)) {
                log_result(&result, unknown_reason, &chord);
                last_logged_result = result;
                have_logged_result = true;
                last_result_log_ms = block_timestamp_ms;
            }
        }

        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (calibrated && now_ms - last_diagnostic_ms >= MUSIC_DIAGNOSTIC_INTERVAL_MS) {
            last_diagnostic_ms = now_ms;
#if MUSIC_USE_SINGLE_MIC_CH1
            diagnostics_log_audio(metrics[0].rms, metrics[0].peak, metrics[0].clip_rate,
                                  0.0f, 0.0f, 0.0f, 1, preprocess.noise_floor, 0.0f,
                                  preprocess.noise_gate, 0.0f);
            ESP_LOGI("DSP_DIAG", "yin_conf=%.2f poly_conf=%.2f tuning=%+.1fc",
                     last_yin_confidence, last_chord_confidence,
                     note_tuning_offset_cents());
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
            ESP_LOGI("DUAL_MIC", "health=%s MIC1=%s score=%.2f snr=%.1fdB streak=%u "
                     "MIC2=%s score=%.2f snr=%.1fdB streak=%u selected=MIC%d switches=%" PRIu32,
                     dual_mic_health_name(mic_selector.health),
                     dual_mic_channel_state_name(mic_selector.quality[0].state),
                     mic_selector.quality[0].score, mic_selector.quality[0].snr_db,
                     mic_selector.quality[0].valid_streak,
                     dual_mic_channel_state_name(mic_selector.quality[1].state),
                     mic_selector.quality[1].score, mic_selector.quality[1].snr_db,
                     mic_selector.quality[1].valid_streak, selected_mic,
                     mic_selector.switch_count);
            ESP_LOGI("DSP_DIAG", "yin_conf=%.2f poly_conf=%.2f tuning=%+.1fc",
                     last_yin_confidence, last_chord_confidence,
                     note_tuning_offset_cents());
            if (!startup_health_logged) {
                startup_health_logged = true;
                ESP_LOGI("DUAL_MIC", "startup signal check: MIC1 rms=%.5f peak=%.3f clip=%.4f valid=%s; "
                         "MIC2 rms=%.5f peak=%.3f clip=%.4f valid=%s",
                         metrics[0].rms, metrics[0].peak, metrics[0].clip_rate,
                         mic_selector.quality[0].signal_valid ? "yes" : "no",
                         metrics[1].rms, metrics[1].peak, metrics[1].clip_rate,
                         mic_selector.quality[1].signal_valid ? "yes" : "no");
            }
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
        counters->dsp_cycle_sum_us += counters->dsp_cycle_time_us;
        ++counters->dsp_cycle_count;
        if (counters->dsp_cycle_time_us > counters->dsp_cycle_max_us) {
            counters->dsp_cycle_max_us = counters->dsp_cycle_time_us;
        }
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
