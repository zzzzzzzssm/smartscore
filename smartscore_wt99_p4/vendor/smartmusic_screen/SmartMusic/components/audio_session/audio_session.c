#include "audio_session.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "app_state.h"
#include "audio_capture.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "note_utils.h"
#include "pitch_detect.h"
#include "pitch_detect_fft.h"
#include "score_engine.h"

static const char *TAG = "audio_session";

/* ── 阈值与参数 ── */
#define AUDIO_RMS_THRESHOLD 50.0f
#define AUDIO_CALIBRATION_SEC 0.50f
#define AUDIO_CALIBRATION_IGNORE_SEC 0.10f
#define AUDIO_START_RMS_MULTIPLIER 2.2f
#define AUDIO_START_DELTA_THRESHOLD 700.0f
#define AUDIO_DYNAMIC_THRESHOLD_MAX 200.0f
#define AUDIO_NOISY_FLOOR_RMS 6000.0f
#define AUDIO_NOISY_DELTA_RMS 200.0f
#define AUDIO_MIN_PITCH_CONFIDENCE 0.12f
#define AUDIO_PITCH_FRAME_INTERVAL_SEC 0.025f
#define AUDIO_NOTE_RECORD_INTERVAL_SEC 0.10f
#define AUDIO_TARGET_PITCH_TOL 1.25f
#define AUDIO_DISPLAY_PITCH_TOL 2.0f
#define AUDIO_DISPLAY_NOISE_REJECT_TOL 4.0f
#define AUDIO_DISPLAY_MIN_CONFIDENCE 0.18f
#define AUDIO_PITCH_PREVIEW_EARLY_SEC 0.08f
#define AUDIO_PITCH_PREVIEW_LATE_SEC 0.30f

#define AUDIO_LONG_FRAME_MAX_MIDI 55
#define AUDIO_LONG_FRAME_SAMPLES (AUDIO_CAPTURE_FRAME_SAMPLES * 2)

#define AUDIO_SLOT_MAX SCORE_MAX_TARGET_NOTES
#define AUDIO_LOCAL_FRAME_MAX SCORE_MAX_PITCH_FRAMES

/* ── 回调 ── */
static audio_session_note_cb_t s_note_cb = NULL;
static audio_session_score_cb_t s_score_cb = NULL;
static audio_session_status_cb_t s_status_cb = NULL;

/* ── 音槽与帧缓冲区 ── */
typedef struct {
    int midi;
    float start;
    float duration;
    bool attack;
    uint16_t source_index;
} audio_target_slot_t;

static int16_t s_audio_samples[AUDIO_CAPTURE_FRAME_SAMPLES];
static int16_t s_long_audio[AUDIO_LONG_FRAME_SAMPLES];
static int s_long_fill;
static target_note_t s_targets[SCORE_MAX_TARGET_NOTES];
static audio_target_slot_t s_slots[AUDIO_SLOT_MAX];
static pitch_frame_t s_frames[AUDIO_LOCAL_FRAME_MAX];
static int s_frame_count;
static int s_slot_count;
static int s_target_count;
static int s_next_slot;
static int s_preview_slot;
static bool s_slot_pitch_reported[AUDIO_SLOT_MAX];
static bool s_score_pending;

static float clamp_float(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static const char *phase_name(app_record_phase_t phase)
{
    switch (phase) {
    case APP_RECORD_PHASE_CALIBRATING: return "CALIBRATING";
    case APP_RECORD_PHASE_WAITING:     return "WAITING";
    case APP_RECORD_PHASE_RECORDING:   return "RECORDING";
    case APP_RECORD_PHASE_IDLE:
    default:                           return "IDLE";
    }
}

/* ── 零交叉频率估计（备胎算法） ── */
static float estimate_freq_zero_crossing(const int16_t *samples, int n, int sample_rate)
{
    if (samples == NULL || n < 8 || sample_rate <= 0) return 0.0f;

    double sum = 0.0;
    for (int i = 0; i < n; ++i) sum += samples[i];
    double mean = sum / (double)n;

    int crossings = 0;
    int prev_positive = samples[0] > mean;
    for (int i = 1; i < n; ++i) {
        int positive = samples[i] > mean;
        if (positive != prev_positive) { crossings++; prev_positive = positive; }
    }

    if (crossings < 2) return 0.0f;
    float freq = ((float)crossings * 0.5f) / ((float)n / (float)sample_rate);
    if (freq < PITCH_MIN_FREQ_HZ || freq > PITCH_MAX_FREQ_HZ) return 0.0f;
    return freq;
}

static void normalize_pitch(float *freq, int *midi)
{
    if (freq == NULL || midi == NULL) return;
    *midi = freq_to_midi(*freq);
    if (*freq < PITCH_MIN_FREQ_HZ || *freq > PITCH_MAX_FREQ_HZ || *midi < 21 || *midi > 108) {
        *freq = 0.0f;
        *midi = -1;
    }
}

/* ── FFT-HPS 与 YIN 加权融合的音高检测 ── */
/*
 * 策略说明：
 * - YIN 是经过验证的时域算法，对乐音基频检测可靠。
 * - FFT+HPS 在谐波丰富时理论上更好，但当前实现有假阳性（锁死 ~554Hz）。
 * - 当两者分歧时，YIN 优先（YIN 置信度 ≥ 0.3 即采信）。
 * - FFT+HPS 仅在 YIN 完全失败时作为补充。
 */
#define FUSION_MIDI_TOL 1.0f      /* 两种算法相差 ≤ 1 半音才认为一致 */
#define FUSION_YIN_MIN_CONF 0.30f /* YIN 最低可信置信度 */

static bool detect_pitch(const int16_t *samples, int n, float *out_freq,
                         int *out_midi, float *out_confidence)
{
    float confidence = 0.0f;
    float freq = 0.0f;

    /* 1. FFT+HPS 检测（参考，暂作辅助） */
    float fft_conf = 0.0f;
    float fft_freq = pitch_detect_fft_hps(samples, n, AUDIO_CAPTURE_SAMPLE_RATE, &fft_conf);

    /* 2. YIN 检测（主要路径，已充分验证） */
    float yin_conf = 0.0f;
    float yin_freq = pitch_detect_yin(samples, n, AUDIO_CAPTURE_SAMPLE_RATE, &yin_conf);

    /* 3. 融合策略：YIN 优先，FFT+HPS 辅助验证 */
    if (yin_freq > 0.0f && yin_conf >= FUSION_YIN_MIN_CONF) {
        /* YIN 检测可靠 → 以 YIN 为主 */
        freq = yin_freq;
        confidence = yin_conf;

        /* 如果 FFT+HPS 也检测到相近频率，适当提升置信度 */
        if (fft_freq > 0.0f && fft_conf > 0.0f) {
            int yin_midi = freq_to_midi(yin_freq);
            int fft_midi = freq_to_midi(fft_freq);
            float delta = (float)yin_midi - (float)fft_midi;
            if (delta < 0.0f) delta = -delta;
            if (delta <= FUSION_MIDI_TOL) {
                confidence = yin_conf + 0.5f * (1.0f - yin_conf) * fft_conf;
                if (confidence > 0.92f) confidence = 0.92f;
            }
        }
    } else if (fft_freq > 0.0f) {
        /* YIN 不可靠 → 用 FFT+HPS */
        freq = fft_freq;
        confidence = fft_conf;
    }

    /* 4. 以上全失败 → 备胎 */
    if (freq <= 0.0f) {
        freq = pitch_detect_autocorr(samples, n, AUDIO_CAPTURE_SAMPLE_RATE);
        if (freq > 0.0f) confidence = 0.28f;
    }
    if (freq <= 0.0f) {
        freq = estimate_freq_zero_crossing(samples, n, AUDIO_CAPTURE_SAMPLE_RATE);
        if (freq > 0.0f) confidence = 0.15f;
    }

    int midi = -1;
    normalize_pitch(&freq, &midi);

    if (out_freq) *out_freq = freq;
    if (out_midi) *out_midi = midi;
    if (out_confidence) *out_confidence = confidence;

    return freq > 0.0f && midi >= 21 && midi <= 108 && confidence >= AUDIO_MIN_PITCH_CONFIDENCE;
}

/* ── 构建音槽 ── */
static int build_target_slots(void)
{
    int bpm = 120;
    app_state_copy_score_meta(NULL, 0, &bpm);
    if (bpm <= 0) bpm = 120;

    int target_count = app_state_copy_targets(s_targets, SCORE_MAX_TARGET_NOTES);
    float beat_sec = 60.0f / (float)bpm;
    if (beat_sec < 0.12f || beat_sec > 2.0f) beat_sec = 0.5f;

    int slot_count = 0;
    for (int i = 0; i < target_count && slot_count < AUDIO_SLOT_MAX; ++i) {
        float duration = s_targets[i].duration > 0.01f ? s_targets[i].duration : beat_sec;
        int repeats = (int)floorf((duration / beat_sec) + 0.35f);
        if (repeats < 1) repeats = 1;
        for (int r = 0; r < repeats && slot_count < AUDIO_SLOT_MAX; ++r) {
            s_slots[slot_count].midi = s_targets[i].midi;
            s_slots[slot_count].start = s_targets[i].start + (float)r * beat_sec;
            s_slots[slot_count].duration = repeats > 1 ? beat_sec : duration;
            s_slots[slot_count].attack = (r == 0);
            s_slots[slot_count].source_index = (uint16_t)i;
            slot_count++;
        }
    }

    s_slot_count = slot_count;
    s_target_count = target_count;
    s_next_slot = 0;
    s_preview_slot = 0;
    s_frame_count = 0;
    memset(s_slot_pitch_reported, 0, sizeof(s_slot_pitch_reported));
    ESP_LOGI(TAG, "target slots: targets=%d slots=%d bpm=%d", target_count, slot_count, bpm);
    return slot_count;
}

static void remember_frame(const pitch_frame_t *frame)
{
    if (!frame || frame->midi < 0) return;
    if (s_frame_count < AUDIO_LOCAL_FRAME_MAX)
        s_frames[s_frame_count++] = *frame;
    app_state_add_pitch_frame(frame);
}

static bool frame_matches(const pitch_frame_t *frame, int midi, float tol)
{
    return frame && frame->midi >= 0 && abs(frame->midi - midi) <= tol;
}

static float midi_abs_delta(int a, int b)
{
    float d = (float)a - (float)b;
    return d < 0.0f ? -d : d;
}

static bool has_matching_frame(float start, float end, int midi)
{
    for (int i = 0; i < s_frame_count; ++i)
        if (s_frames[i].time >= start && s_frames[i].time <= end &&
            frame_matches(&s_frames[i], midi, AUDIO_TARGET_PITCH_TOL))
            return true;
    return false;
}

static int dominant_midi_for_window(float start, float end, float *out_first, float *out_conf)
{
    int counts[128] = {0};
    float conf_sum[128] = {0};
    float first = -1.0f;

    for (int i = 0; i < s_frame_count; ++i) {
        const pitch_frame_t *f = &s_frames[i];
        if (f->time < start || f->time > end || f->midi < 0 || f->midi > 127) continue;
        counts[f->midi]++;
        conf_sum[f->midi] += f->confidence;
        if (first < 0.0f) first = f->time;
    }

    int best = -1, best_cnt = 0;
    for (int m = 0; m < 128; ++m)
        if (counts[m] > best_cnt) { best_cnt = counts[m]; best = m; }

    if (out_first) *out_first = first;
    if (out_conf) *out_conf = (best >= 0 && best_cnt > 0) ? conf_sum[best] / (float)best_cnt : 0.0f;
    return best;
}

static void send_note_result(int slot_index, int played_midi, float played_start,
                              float confidence, bool preview)
{
    const audio_target_slot_t *slot = &s_slots[slot_index];
    bool observed = played_midi >= 0;
    bool pitch_ok = observed && abs(played_midi - slot->midi) <= AUDIO_TARGET_PITCH_TOL;
    bool rhythm_ok = preview ? pitch_ok : pitch_ok;

    if (!preview && pitch_ok && played_start >= 0.0f) {
        float late = played_start - slot->start;
        if (late > 0.30f && late > slot->duration * 0.55f) rhythm_ok = false;
    }
    if (!preview && pitch_ok && slot->attack && slot_index > 0) {
        bool same_before = has_matching_frame(slot->start - 0.20f, slot->start - 0.02f, slot->midi);
        bool same_after = has_matching_frame(slot->start + 0.02f, slot->start + 0.20f, slot->midi);
        bool prev_same = s_slots[slot_index - 1].midi == slot->midi;
        if (same_before && same_after && prev_same) rhythm_ok = false;
    }

    if (s_note_cb) {
        s_note_cb(slot->source_index + 1, slot->midi, played_midi, confidence, pitch_ok, rhythm_ok);
    }

    ESP_LOGI(TAG, "%s slot %d/%d expected=%d played=%d pitch_ok=%d",
             preview ? "preview" : "final",
             slot->source_index + 1, s_target_count, slot->midi, played_midi, pitch_ok);
}

static void evaluate_slot(int slot_index)
{
    const audio_target_slot_t *slot = &s_slots[slot_index];
    float ws = slot->start - 0.10f;
    float we = slot->start + slot->duration + 0.10f;
    if (ws < 0.0f) ws = 0.0f;

    float first_time = -1.0f, confidence = 0.0f;
    int midi = dominant_midi_for_window(ws, we, &first_time, &confidence);
    send_note_result(slot_index, midi, first_time, confidence, false);
    s_slot_pitch_reported[slot_index] = true;
    if (s_preview_slot <= slot_index) s_preview_slot = slot_index + 1;
}

/* ── 通知评分完成 ── */
static void notify_score_complete(void)
{
    char *json = app_state_get_result_json();
    if (json && s_score_cb) {
        s_score_cb(json);
    }
    free(json);
}

/* ── 通知状态更新 ── */
static void notify_status(void)
{
    char *json = app_state_build_status_json();
    if (json && s_status_cb) {
        s_status_cb(json);
    }
    free(json);
}

/* ── 主要任务 ── */
static void audio_session_task(void *arg)
{
    (void)arg;

    bool session_active = false;
    bool calibration_done = false;
    float calibration_sum = 0.0f;
    int calibration_count = 0;
    float noise_floor = 0.0f;
    float dynamic_threshold = AUDIO_RMS_THRESHOLD;
    float performance_start_time = -1.0f;
    float last_frame_time = -10.0f;
    float last_note_time = -10.0f;
    float next_wait_log_time = 0.0f;
    float next_no_frame_log_time = 0.0f;
    float next_status_time = 0.0f;
    bool window_seeded = false;

    while (1) {
        if (!app_state_is_recording()) {
            session_active = false;
            calibration_done = false;
            calibration_sum = 0.0f;
            calibration_count = 0;
            noise_floor = 0.0f;
            dynamic_threshold = AUDIO_RMS_THRESHOLD;
            performance_start_time = -1.0f;
            last_frame_time = -10.0f;
            last_note_time = -10.0f;
            next_wait_log_time = 0.0f;
            next_no_frame_log_time = 0.0f;
            next_status_time = 0.0f;
            window_seeded = false;
            s_long_fill = 0;
            s_frame_count = 0;
            s_slot_count = 0;
            s_target_count = 0;
            s_next_slot = 0;
            s_preview_slot = 0;
            if (s_score_pending) {
                s_score_pending = false;
                notify_score_complete();
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!session_active) {
            session_active = true;
            calibration_done = false;
            calibration_sum = 0.0f;
            calibration_count = 0;
            noise_floor = 0.0f;
            dynamic_threshold = AUDIO_RMS_THRESHOLD;
            performance_start_time = -1.0f;
            last_frame_time = -10.0f;
            last_note_time = -10.0f;
            next_wait_log_time = 0.0f;
            next_no_frame_log_time = 0.0f;
            next_status_time = 0.0f;
            window_seeded = false;
            s_long_fill = 0;
            s_score_pending = false;

            if (build_target_slots() <= 0) {
                ESP_LOGW(TAG, "no targets; finishing");
                app_state_stop_recording();
                continue;
            }
            ESP_LOGI(TAG, "session active: slots=%d first_midi=%d", s_slot_count, s_slots[0].midi);
            app_state_set_record_phase(APP_RECORD_PHASE_CALIBRATING);
            app_state_update_noise_status(noise_floor, dynamic_threshold);
            notify_status();
        }

        float now_sec = app_state_record_time_sec();
        if (now_sec >= next_status_time) {
            notify_status();
            next_status_time = now_sec + 0.50f;
        }

        /* 50% 重叠滑动窗口 */
        int half_frame = AUDIO_CAPTURE_FRAME_SAMPLES / 2;
        int n;
        if (!window_seeded) {
            n = audio_capture_read_frame(s_audio_samples, AUDIO_CAPTURE_FRAME_SAMPLES);
            window_seeded = (n > 0);
        } else {
            memmove(s_audio_samples, s_audio_samples + half_frame, half_frame * sizeof(int16_t));
            memset(s_audio_samples + half_frame, 0, half_frame * sizeof(int16_t));
            int half_n = audio_capture_read_frame(s_audio_samples + half_frame, half_frame);
            n = half_n > 0 ? AUDIO_CAPTURE_FRAME_SAMPLES : 0;
        }

        if (n <= 0) {
            if (now_sec >= next_no_frame_log_time) {
                ESP_LOGW(TAG, "no audio frames: t=%.2f phase=%s", now_sec, phase_name(app_state_get_record_phase()));
                next_no_frame_log_time = now_sec + 1.0f;
            }
            if (!calibration_done && now_sec >= AUDIO_CALIBRATION_SEC) {
                calibration_done = true;
                noise_floor = calibration_count > 0 ? noise_floor : 0.0f;
                dynamic_threshold = AUDIO_RMS_THRESHOLD;
                app_state_update_noise_status(noise_floor, dynamic_threshold);
                app_state_set_record_phase(APP_RECORD_PHASE_WAITING);
                notify_status();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* 低频长帧处理 */
        int current_n = n;
        int16_t *current_samples = s_audio_samples;
        if (s_next_slot < s_slot_count && s_slots[s_next_slot].midi > 0 &&
            s_slots[s_next_slot].midi <= AUDIO_LONG_FRAME_MAX_MIDI) {
            memcpy(s_long_audio + s_long_fill, s_audio_samples, n * sizeof(int16_t));
            s_long_fill += n;
            if (s_long_fill >= AUDIO_LONG_FRAME_SAMPLES) {
                current_n = AUDIO_LONG_FRAME_SAMPLES;
                current_samples = s_long_audio;
                s_long_fill = AUDIO_CAPTURE_FRAME_SAMPLES;
                memmove(s_long_audio, s_long_audio + AUDIO_CAPTURE_FRAME_SAMPLES,
                        AUDIO_CAPTURE_FRAME_SAMPLES * sizeof(int16_t));
            }
        } else {
            s_long_fill = 0;
        }

        float rms, freq = 0.0f;
        int midi = -1;
        float confidence = 0.0f;
        bool pitch_ok = false;

        if (s_long_fill > 0 && s_long_fill < AUDIO_LONG_FRAME_SAMPLES) {
            rms = audio_calculate_rms(s_audio_samples, n);
        } else {
            rms = audio_calculate_rms(current_samples, current_n);
            if (rms > AUDIO_RMS_THRESHOLD) {
                pitch_ok = detect_pitch(current_samples, current_n, &freq, &midi, &confidence);
            }
        }
        app_state_update_audio_status(rms, freq, midi, confidence);

        /* 校准 */
        if (!calibration_done) {
            if (now_sec >= AUDIO_CALIBRATION_IGNORE_SEC) {
                calibration_sum += rms;
                calibration_count++;
                noise_floor = calibration_count > 0 ? calibration_sum / (float)calibration_count : rms;
            }
            if (now_sec >= AUDIO_CALIBRATION_SEC) {
                calibration_done = true;
                float raw_threshold;
                if (noise_floor > AUDIO_NOISY_FLOOR_RMS) {
                    raw_threshold = noise_floor + AUDIO_NOISY_DELTA_RMS;
                    if (raw_threshold > 25000.0f) raw_threshold = AUDIO_RMS_THRESHOLD;
                } else {
                    float by_mul = noise_floor * AUDIO_START_RMS_MULTIPLIER;
                    float by_del = noise_floor + AUDIO_START_DELTA_THRESHOLD;
                    raw_threshold = by_mul > by_del ? by_mul : by_del;
                }
                dynamic_threshold = clamp_float(raw_threshold, AUDIO_RMS_THRESHOLD, AUDIO_DYNAMIC_THRESHOLD_MAX);
                app_state_update_noise_status(noise_floor, dynamic_threshold);
                app_state_set_record_phase(APP_RECORD_PHASE_WAITING);
                notify_status();
                ESP_LOGI(TAG, "calibration: noise=%.1f threshold=%.1f", noise_floor, dynamic_threshold);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* 等待起奏 */
        app_record_phase_t phase = app_state_get_record_phase();
        if (phase == APP_RECORD_PHASE_WAITING) {
            bool first_candidate = pitch_ok;
            bool first_matches = first_candidate && midi_abs_delta(midi, s_slots[0].midi) <= AUDIO_DISPLAY_NOISE_REJECT_TOL;
            if (now_sec >= next_wait_log_time) {
                ESP_LOGI(TAG, "waiting: target=%d rms=%.1f midi=%d conf=%.2f match=%d",
                         s_slots[0].midi, rms, midi, confidence, first_matches);
                next_wait_log_time = now_sec + 1.0f;
            }
            if (first_matches) {
                performance_start_time = now_sec - s_slots[0].start;
                if (performance_start_time < 0.0f) performance_start_time = now_sec;
                app_state_mark_performance_started(performance_start_time);
                notify_status();
                float rel_time = now_sec - performance_start_time;
                if (rel_time < 0.0f) rel_time = 0.0f;
                pitch_frame_t frame = { .time = rel_time, .midi = midi, .freq = freq, .confidence = confidence, .rms = rms };
                remember_frame(&frame);
                last_frame_time = rel_time;
                ESP_LOGI(TAG, "performance started at %.3f midi=%d", performance_start_time, midi);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* 正式录音 */
        if (phase == APP_RECORD_PHASE_RECORDING && performance_start_time >= 0.0f) {
            float rel_time = now_sec - performance_start_time;
            if (rel_time < 0.0f) rel_time = 0.0f;

            if (pitch_ok && rms > dynamic_threshold &&
                (rel_time - last_frame_time) >= AUDIO_PITCH_FRAME_INTERVAL_SEC) {
                pitch_frame_t frame = { .time = rel_time, .midi = midi, .freq = freq, .confidence = confidence, .rms = rms };
                remember_frame(&frame);
                last_frame_time = rel_time;
            }

            if (pitch_ok && rms > dynamic_threshold &&
                (rel_time - last_note_time) >= AUDIO_NOTE_RECORD_INTERVAL_SEC) {
                played_note_t note = { .midi = midi, .start = rel_time, .duration = AUDIO_NOTE_RECORD_INTERVAL_SEC,
                                       .freq = freq, .confidence = confidence };
                if (app_state_add_played_note(&note))
                    last_note_time = rel_time;
            }

            while (s_next_slot < s_slot_count &&
                   rel_time >= s_slots[s_next_slot].start + s_slots[s_next_slot].duration + 0.08f) {
                evaluate_slot(s_next_slot);
                s_next_slot++;
            }

            if (s_next_slot >= s_slot_count &&
                rel_time >= s_slots[s_slot_count - 1].start + s_slots[s_slot_count - 1].duration + 0.60f) {
                ESP_LOGI(TAG, "all slots evaluated; finishing");
                app_state_stop_recording();
                s_score_pending = true;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ── 公开 API ── */
void audio_session_register_note_callback(audio_session_note_cb_t cb) { s_note_cb = cb; }
void audio_session_register_score_callback(audio_session_score_cb_t cb) { s_score_cb = cb; }
void audio_session_register_status_callback(audio_session_status_cb_t cb) { s_status_cb = cb; }

esp_err_t audio_session_init(void)
{
    return ESP_OK;
}

esp_err_t audio_session_start(void)
{
    BaseType_t ok = xTaskCreate(audio_session_task, "audio_session", 16384, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create audio_session task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "audio_session task started");
    return ESP_OK;
}
