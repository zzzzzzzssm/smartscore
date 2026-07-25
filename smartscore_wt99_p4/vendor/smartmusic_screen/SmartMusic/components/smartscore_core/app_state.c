#include "app_state.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "note_utils.h"
#include "sdkconfig.h"

static const char *TAG = "app_state";

/*
 * s_lock 保护下面所有全局状态。
 * ESP32 上 web_server 和 audio_task 是不同任务，不加锁可能会一边写一边读。
 */
static SemaphoreHandle_t s_lock;
static app_record_state_t s_state = APP_STATE_IDLE;
static app_record_phase_t s_record_phase = APP_RECORD_PHASE_IDLE;

static char s_title[64] = "untitled";
static int s_bpm = 120;
static target_note_t s_targets[SCORE_MAX_TARGET_NOTES];
static int s_target_count;

static played_note_t s_played[SCORE_MAX_PLAYED_NOTES];
static int s_played_count;
static pitch_frame_t s_pitch_frames[SCORE_MAX_PITCH_FRAMES];
static int s_pitch_frame_count;

static int64_t s_record_start_us;
static float s_last_rms;
static float s_last_freq;
static float s_last_confidence;
static int s_last_midi = -1;
static float s_noise_floor;
static float s_dynamic_threshold;
static bool s_noise_warning;
static float s_performance_start_delay = -1.0f;
static size_t s_last_sheet_image_size;
static char s_message[96] = "idle";
static char *s_last_result_json;

#define AI_CONTEXT_MAX_NOTES SCORE_MAX_TARGET_NOTES
#define AI_CONTEXT_INITIAL_BYTES (24 * 1024)
#define AI_CONTEXT_MAX_BYTES (192 * 1024)

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool failed;
} ai_json_builder_t;

static char *large_heap_alloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }
#ifdef CONFIG_SPIRAM
    char *psram = (char *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (psram != NULL) {
        return psram;
    }
#endif
    char *heap = (char *)heap_caps_malloc(size, MALLOC_CAP_8BIT);
    return heap != NULL ? heap : (char *)malloc(size);
}

static bool ai_json_reserve(ai_json_builder_t *builder, size_t needed)
{
    if (builder == NULL || builder->failed) {
        return false;
    }
    if (needed <= builder->cap) {
        return true;
    }
    if (needed > AI_CONTEXT_MAX_BYTES) {
        builder->failed = true;
        return false;
    }

    size_t next_cap = builder->cap == 0 ? AI_CONTEXT_INITIAL_BYTES : builder->cap;
    while (next_cap < needed && next_cap < AI_CONTEXT_MAX_BYTES) {
        next_cap *= 2;
    }
    if (next_cap > AI_CONTEXT_MAX_BYTES) {
        next_cap = AI_CONTEXT_MAX_BYTES;
    }
    if (next_cap < needed) {
        builder->failed = true;
        return false;
    }

    char *next = large_heap_alloc(next_cap);
    if (next == NULL) {
        builder->failed = true;
        return false;
    }
    if (builder->data != NULL && builder->len > 0) {
        memcpy(next, builder->data, builder->len);
    }
    free(builder->data);
    builder->data = next;
    builder->cap = next_cap;
    builder->data[builder->len] = '\0';
    return true;
}

static bool ai_json_append_raw(ai_json_builder_t *builder, const char *text)
{
    if (builder == NULL || text == NULL || builder->failed) {
        return false;
    }
    size_t text_len = strlen(text);
    size_t needed = builder->len + text_len + 1;
    if (!ai_json_reserve(builder, needed)) {
        return false;
    }
    memcpy(builder->data + builder->len, text, text_len + 1);
    builder->len += text_len;
    return true;
}

static bool ai_json_appendf(ai_json_builder_t *builder, const char *fmt, ...)
{
    if (builder == NULL || fmt == NULL || builder->failed) {
        return false;
    }

    while (1) {
        if (!ai_json_reserve(builder, builder->len + 96)) {
            return false;
        }
        va_list args;
        va_start(args, fmt);
        int written = vsnprintf(builder->data + builder->len,
                                builder->cap - builder->len,
                                fmt,
                                args);
        va_end(args);
        if (written < 0) {
            builder->failed = true;
            return false;
        }
        size_t needed = builder->len + (size_t)written + 1;
        if (needed <= builder->cap) {
            builder->len += (size_t)written;
            return true;
        }
        if (!ai_json_reserve(builder, needed)) {
            return false;
        }
    }
}

static bool ai_json_append_escaped(ai_json_builder_t *builder, const char *text)
{
    if (!ai_json_append_raw(builder, "\"")) {
        return false;
    }
    const char *p = text != NULL ? text : "";
    while (*p != '\0') {
        unsigned char ch = (unsigned char)*p++;
        switch (ch) {
        case '\\':
            if (!ai_json_append_raw(builder, "\\\\")) {
                return false;
            }
            break;
        case '"':
            if (!ai_json_append_raw(builder, "\\\"")) {
                return false;
            }
            break;
        case '\n':
            if (!ai_json_append_raw(builder, "\\n")) {
                return false;
            }
            break;
        case '\r':
            if (!ai_json_append_raw(builder, "\\r")) {
                return false;
            }
            break;
        case '\t':
            if (!ai_json_append_raw(builder, "\\t")) {
                return false;
            }
            break;
        default:
            if (ch < 0x20) {
                if (!ai_json_appendf(builder, "\\u%04x", ch)) {
                    return false;
                }
            } else {
                char one[2] = { (char)ch, '\0' };
                if (!ai_json_append_raw(builder, one)) {
                    return false;
                }
            }
            break;
        }
    }
    return ai_json_append_raw(builder, "\"");
}

/* ESP-IDF 环境里为了兼容性，自己实现一个字符串复制函数。 */
static char *dup_string(const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (copy != NULL) {
        memcpy(copy, s, len);
    }
    return copy;
}

static const char *state_to_string(app_record_state_t state)
{
    /* 把枚举转成网页 JSON 里更容易看的字符串。 */
    switch (state) {
    case APP_STATE_IDLE:
        return "IDLE";
    case APP_STATE_READY:
        return "READY";
    case APP_STATE_RECORDING:
        return "RECORDING";
    case APP_STATE_FINISHED:
        return "FINISHED";
    default:
        return "UNKNOWN";
    }
}

static const char *phase_to_string(app_record_phase_t phase)
{
    /* 录音内部阶段字符串，网页“当前阶段”会显示它。 */
    switch (phase) {
    case APP_RECORD_PHASE_CALIBRATING:
        return "CALIBRATING";
    case APP_RECORD_PHASE_WAITING:
        return "WAITING";
    case APP_RECORD_PHASE_RECORDING:
        return "RECORDING";
    case APP_RECORD_PHASE_IDLE:
    default:
        return "IDLE";
    }
}

static void lock_state(void)
{
    /* 所有读写共享状态的函数都先拿锁。 */
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock_state(void)
{
    /* 和 lock_state 成对出现。 */
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

esp_err_t app_state_init(void)
{
    /* 创建互斥锁，只需要初始化一次。 */
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

void app_state_clear_all(void)
{
    /* 清空所有运行时数据，相当于让系统回到刚启动状态。 */
    lock_state();
    s_state = APP_STATE_IDLE;
    s_record_phase = APP_RECORD_PHASE_IDLE;
    strncpy(s_title, "untitled", sizeof(s_title) - 1);
    s_title[sizeof(s_title) - 1] = '\0';
    s_bpm = 120;
    s_target_count = 0;
    s_played_count = 0;
    s_pitch_frame_count = 0;
    s_record_start_us = 0;
    s_last_rms = 0.0f;
    s_last_freq = 0.0f;
    s_last_confidence = 0.0f;
    s_last_midi = -1;
    s_noise_floor = 0.0f;
    s_dynamic_threshold = 0.0f;
    s_performance_start_delay = -1.0f;
    s_last_sheet_image_size = 0;
    snprintf(s_message, sizeof(s_message), "cleared");
    free(s_last_result_json);
    s_last_result_json = NULL;
    unlock_state();
}

esp_err_t app_state_set_score(const char *title,
                              int bpm,
                              const target_note_t *notes,
                              int note_count,
                              int *stored_count,
                              bool *truncated)
{
    /* 网页上传的 score.json 会最终进入这个函数。 */
    if (notes == NULL || note_count <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* ESP32 内存有限，最多只保存 SCORE_MAX_TARGET_NOTES 个标准音符。 */
    int count = note_count;
    bool was_truncated = false;
    if (count > SCORE_MAX_TARGET_NOTES) {
        count = SCORE_MAX_TARGET_NOTES;
        was_truncated = true;
    }

    lock_state();
    /* 保存标题、BPM、标准音符数组，并清空上一轮演奏/评分结果。 */
    if (title != NULL && title[0] != '\0') {
        strncpy(s_title, title, sizeof(s_title) - 1);
        s_title[sizeof(s_title) - 1] = '\0';
    } else {
        strncpy(s_title, "untitled", sizeof(s_title) - 1);
        s_title[sizeof(s_title) - 1] = '\0';
    }
    s_bpm = bpm > 0 ? bpm : 120;
    memcpy(s_targets, notes, sizeof(target_note_t) * count);
    s_target_count = count;
    s_played_count = 0;
    s_pitch_frame_count = 0;
    s_state = APP_STATE_READY;
    s_record_phase = APP_RECORD_PHASE_IDLE;
    s_last_rms = 0.0f;
    s_last_freq = 0.0f;
    s_last_confidence = 0.0f;
    s_last_midi = -1;
    s_noise_floor = 0.0f;
    s_dynamic_threshold = 0.0f;
    s_performance_start_delay = -1.0f;
    s_last_sheet_image_size = 0;
    free(s_last_result_json);
    s_last_result_json = NULL;
    snprintf(s_message,
             sizeof(s_message),
             was_truncated ? "score uploaded, truncated to 1024 notes" : "score uploaded");
    unlock_state();

    if (stored_count != NULL) {
        *stored_count = count;
    }
    if (truncated != NULL) {
        *truncated = was_truncated;
    }
    return ESP_OK;
}

esp_err_t app_state_start_recording(void)
{
    /* 没上传乐谱不允许开始，否则后面评分没有 target。 */
    lock_state();
    if (s_target_count <= 0) {
        snprintf(s_message, sizeof(s_message), "请先上传标准乐谱");
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }

    s_played_count = 0;
    s_pitch_frame_count = 0;
    /* 记录开始时间，audio_task 会用它计算每个音符的 start。 */
    s_record_start_us = esp_timer_get_time();
    s_last_rms = 0.0f;
    s_last_freq = 0.0f;
    s_last_confidence = 0.0f;
    s_last_midi = -1;
    s_noise_floor = 0.0f;
    s_dynamic_threshold = 0.0f;
    s_performance_start_delay = -1.0f;
    /*
     * 点击开始后不立刻记录音符，而是先进入 CALIBRATING。
     * audio_task 会在这个阶段只统计环境 RMS，避免按钮声、说话声被当成第一个音。
     */
    s_state = APP_STATE_RECORDING;
    s_record_phase = APP_RECORD_PHASE_CALIBRATING;
    s_performance_start_delay = -1.0f;
    snprintf(s_message, sizeof(s_message), "calibrating environment noise");
    free(s_last_result_json);
    s_last_result_json = NULL;
    unlock_state();
    ESP_LOGI(TAG, "recording started");
    return ESP_OK;
}

esp_err_t app_state_force_start_recording(bool *already_recording)
{
    /*
     * 网页“强制开始记录”按钮走这里。
     * 如果系统卡在校准/等待阶段，就把当前时刻当成真正演奏开始时间。
     */
    lock_state();
    if (s_state != APP_STATE_RECORDING) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }

    bool already = s_record_phase == APP_RECORD_PHASE_RECORDING;
    if (!already) {
        float delay = 0.0f;
        if (s_record_start_us > 0) {
            delay = (float)(esp_timer_get_time() - s_record_start_us) / 1000000.0f;
        }
        s_record_phase = APP_RECORD_PHASE_RECORDING;
        s_performance_start_delay = delay;
        snprintf(s_message, sizeof(s_message), "forced recording started");
    }

    if (already_recording != NULL) {
        *already_recording = already;
    }
    unlock_state();
    return ESP_OK;
}

void app_state_stop_recording(void)
{
    /* 仅停止录音状态，不评分，不阻塞。 */
    lock_state();
    s_state = APP_STATE_FINISHED;
    s_record_phase = APP_RECORD_PHASE_IDLE;
    snprintf(s_message, sizeof(s_message), "finished");
    unlock_state();
    ESP_LOGI(TAG, "recording stopped (deferred)");
}

char *app_state_run_scoring(void)
{
    /*
     * 在停止录音后异步执行评分。
     * 复制一份 target/played 数据到堆上，释放锁后再调用评分引擎。
     */
    char title_copy[sizeof(s_title)];
    int target_count = 0;
    int played_count = 0;
    int frame_count = 0;
    float performance_start_delay = -1.0f;

    lock_state();
    target_count = s_target_count;
    played_count = s_played_count;
    frame_count = s_pitch_frame_count;
    performance_start_delay = s_performance_start_delay;
    strncpy(title_copy, s_title, sizeof(title_copy) - 1);
    title_copy[sizeof(title_copy) - 1] = '\0';

    target_note_t *target_copy = NULL;
    played_note_t *played_copy = NULL;
    pitch_frame_t *frame_copy = NULL;
    if (target_count > 0) {
        target_copy = (target_note_t *)large_heap_alloc(sizeof(target_note_t) * target_count);
        if (target_copy != NULL) {
            memcpy(target_copy, s_targets, sizeof(target_note_t) * target_count);
        }
    }
    if (played_count > 0) {
        played_copy = (played_note_t *)large_heap_alloc(sizeof(played_note_t) * played_count);
        if (played_copy != NULL) {
            memcpy(played_copy, s_played, sizeof(played_note_t) * played_count);
        }
    }
    if (frame_count > 0) {
        frame_copy = (pitch_frame_t *)large_heap_alloc(sizeof(pitch_frame_t) * frame_count);
        if (frame_copy != NULL) {
            memcpy(frame_copy, s_pitch_frames, sizeof(pitch_frame_t) * frame_count);
        }
    }
    unlock_state();

    char *json = NULL;
    if (target_count == 0) {
        json = dup_string("{\"ok\":false,\"message\":\"请先上传标准乐谱\"}");
    } else if (target_copy == NULL) {
        json = dup_string("{\"ok\":false,\"message\":\"not enough memory for target score\"}");
    } else if (played_count > 0 && played_copy == NULL) {
        json = dup_string("{\"ok\":false,\"message\":\"not enough memory for played notes\"}");
    } else if (frame_count > 0 && frame_copy == NULL) {
        json = dup_string("{\"ok\":false,\"message\":\"not enough memory for pitch frames\"}");
    } else if (played_count == 0 && frame_count == 0) {
        cJSON *root = cJSON_CreateObject();
        if (root != NULL) {
            cJSON_AddBoolToObject(root, "ok", true);
            cJSON_AddStringToObject(root, "message", "没有识别到有效演奏音符");
            cJSON_AddStringToObject(root, "title", title_copy);
            cJSON_AddNumberToObject(root, "target_count", target_count);
            cJSON_AddNumberToObject(root, "played_count", 0);
            cJSON_AddNumberToObject(root, "raw_played_count", 0);
            cJSON_AddNumberToObject(root, "merged_played_count", 0);
            cJSON_AddNumberToObject(root, "scored_played_count", 0);
            cJSON_AddNumberToObject(root, "pitch_frame_count", 0);
            cJSON_AddNumberToObject(root, "matched_count", 0);
            cJSON_AddNumberToObject(root, "aligned_count", 0);
            cJSON_AddNumberToObject(root, "good_count", 0);
            cJSON_AddNumberToObject(root, "frame_recovered_count", 0);
            cJSON_AddNumberToObject(root, "leading_trim_count", 0);
            cJSON_AddNumberToObject(root, "leading_trim_score", 0);
            cJSON_AddNumberToObject(root, "ignored_played_count", 0);
            cJSON_AddNumberToObject(root, "pitch_score", 0);
            cJSON_AddNumberToObject(root, "rhythm_score", 0);
            cJSON_AddNumberToObject(root, "duration_score", 0);
            cJSON_AddNumberToObject(root, "stability_score", 100);
            cJSON_AddNumberToObject(root, "complete_score", 0);
            cJSON_AddNumberToObject(root, "total_score", 0);
            cJSON_AddStringToObject(root, "alignment_method", "edit_distance_offset_tempo_gate");
            cJSON_AddNumberToObject(root, "tempo_scale", 1.0);
            cJSON_AddNumberToObject(root, "start_offset", 0.0);
            cJSON_AddNumberToObject(root, "performance_start_delay", performance_start_delay);
            cJSON_AddNumberToObject(root, "extra_count", 0);
            cJSON_AddNumberToObject(root, "missing_count", target_count);
            cJSON_AddNumberToObject(root, "low_confidence_count", 0);
            cJSON_AddNumberToObject(root, "pitch_error_count", 0);
            cJSON_AddNumberToObject(root, "rhythm_error_count", 0);
            cJSON_AddNumberToObject(root, "duration_error_count", 0);
            cJSON_AddNumberToObject(root, "extra_penalty", 0);
            cJSON_AddNumberToObject(root, "missing_penalty", (double)target_count * 3.0);
            cJSON_AddNumberToObject(root, "low_confidence_penalty", 0);
            cJSON_AddStringToObject(root, "summary", "没有识别到有效演奏音符，请检查麦克风或增大音量。");
            cJSON *details = cJSON_CreateArray();
            if (details != NULL) {
                cJSON_AddItemToObject(root, "details", details);
            }
            json = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
        }
    } else {
        if (frame_count > 0) {
            json = score_engine_build_frame_result_json(title_copy,
                                                        target_copy,
                                                        target_count,
                                                        frame_copy,
                                                        frame_count,
                                                        played_copy,
                                                        played_count,
                                                        performance_start_delay);
        } else {
            json = score_engine_build_result_json(title_copy,
                                                  target_copy,
                                                  target_count,
                                                  played_copy,
                                                  played_count,
                                                  performance_start_delay);
        }
    }
    if (json == NULL) {
        json = dup_string("{\"ok\":false,\"message\":\"failed to build result\"}");
    }

    free(target_copy);
    free(played_copy);
    free(frame_copy);

    lock_state();
    free(s_last_result_json);
    s_last_result_json = json != NULL ? dup_string(json) : NULL;
    unlock_state();

    ESP_LOGI(TAG, "scoring done, target=%d played=%d frames=%d",
             target_count,
             played_count,
             frame_count);
    return json;
}

char *app_state_stop_and_score(void)
{
    /* 旧接口：先停再评分，同步阻塞。保留兼容。 */
    app_state_stop_recording();
    return app_state_run_scoring();
}

bool app_state_is_recording(void)
{
    /* audio_task 高频调用这个函数，所以逻辑保持很短。 */
    bool recording;
    lock_state();
    recording = s_state == APP_STATE_RECORDING;
    unlock_state();
    return recording;
}

float app_state_record_time_sec(void)
{
    /* 返回从点击“开始记录”到当前的秒数。 */
    float sec = 0.0f;
    lock_state();
    if (s_record_start_us > 0) {
        sec = (float)(esp_timer_get_time() - s_record_start_us) / 1000000.0f;
    }
    unlock_state();
    return sec;
}

int app_state_get_target_count(void)
{
    /* 给 audio_task / 调试日志读取标准音符数量。 */
    int count;
    lock_state();
    count = s_target_count;
    unlock_state();
    return count;
}

bool app_state_get_first_target_midi(int *midi)
{
    /* 保留给智能起奏模式：判断用户是否弹了第一个目标音附近的音。 */
    bool ok = false;
    lock_state();
    if (midi != NULL && s_target_count > 0) {
        *midi = s_targets[0].midi;
        ok = true;
    }
    unlock_state();
    return ok;
}

int app_state_copy_target_prefix(target_note_t *out_notes, int max_count)
{
    if (out_notes == NULL || max_count <= 0) {
        return 0;
    }

    lock_state();
    int count = s_target_count < max_count ? s_target_count : max_count;
    if (count > 0) {
        memcpy(out_notes, s_targets, sizeof(target_note_t) * count);
    }
    unlock_state();
    return count;
}

int app_state_copy_targets(target_note_t *out_notes, int max_count)
{
    if (out_notes == NULL || max_count <= 0) {
        return 0;
    }

    lock_state();
    int count = s_target_count < max_count ? s_target_count : max_count;
    if (count > 0) {
        memcpy(out_notes, s_targets, sizeof(target_note_t) * count);
    }
    unlock_state();
    return count;
}

void app_state_copy_score_meta(char *title, size_t title_size, int *bpm)
{
    lock_state();
    if (title != NULL && title_size > 0) {
        strncpy(title, s_title, title_size - 1);
        title[title_size - 1] = '\0';
    }
    if (bpm != NULL) {
        *bpm = s_bpm;
    }
    unlock_state();
}

void app_state_update_audio_status(float rms, float freq, int midi, float confidence)
{
    /* audio_task 每帧更新，/api/status 会把这些值发给网页。 */
    lock_state();
    s_last_rms = rms;
    s_last_freq = freq;
    s_last_confidence = confidence;
    s_last_midi = midi;
    unlock_state();
}

void app_state_set_record_phase(app_record_phase_t phase)
{
    /* 更新录音阶段，同时写一条人类可读的 message。 */
    lock_state();
    s_record_phase = phase;
    switch (phase) {
    case APP_RECORD_PHASE_CALIBRATING:
        snprintf(s_message, sizeof(s_message), "calibrating environment noise");
        break;
    case APP_RECORD_PHASE_WAITING:
        snprintf(s_message, sizeof(s_message), "waiting for first target note");
        break;
    case APP_RECORD_PHASE_RECORDING:
        snprintf(s_message, sizeof(s_message), "recording performance");
        break;
    case APP_RECORD_PHASE_IDLE:
    default:
        break;
    }
    unlock_state();
}

app_record_phase_t app_state_get_record_phase(void)
{
    /* 只读查询当前阶段，主要给 audio_task 判断是否被 /api/force_start 强制切换。 */
    app_record_phase_t phase;
    lock_state();
    phase = s_record_phase;
    unlock_state();
    return phase;
}

void app_state_update_noise_status(float noise_floor, float dynamic_threshold)
{
    lock_state();
    s_noise_floor = noise_floor;
    s_dynamic_threshold = dynamic_threshold;
    s_noise_warning = noise_floor > 6000.0f;
    unlock_state();
}

void app_state_mark_performance_started(float performance_start_delay)
{
    /* 智能起奏模式中表示真正演奏开始；直接记录模式中通常是 0。 */
    lock_state();
    s_record_phase = APP_RECORD_PHASE_RECORDING;
    s_performance_start_delay = performance_start_delay;
    snprintf(s_message, sizeof(s_message), "recording performance");
    unlock_state();
}

void app_state_set_sheet_image_size(size_t size)
{
    /* 第一版只记录图片大小，不保存图片内容；后续接墨水屏时可以扩展为保存图片缓冲区。 */
    lock_state();
    s_last_sheet_image_size = size;
    unlock_state();
}

bool app_state_add_played_note(const played_note_t *note)
{
    /* 追加一个演奏音符，并防止超过 SCORE_MAX_PLAYED_NOTES。 */
    if (note == NULL) {
        return false;
    }

    bool ok = false;
    lock_state();
    if (s_state == APP_STATE_RECORDING &&
        s_record_phase == APP_RECORD_PHASE_RECORDING &&
        s_played_count < SCORE_MAX_PLAYED_NOTES) {
        s_played[s_played_count++] = *note;
        ok = true;
    } else if (s_played_count >= SCORE_MAX_PLAYED_NOTES) {
        snprintf(s_message, sizeof(s_message), "played note buffer full");
    }
    unlock_state();
    return ok;
}

bool app_state_add_pitch_frame(const pitch_frame_t *frame)
{
    if (frame == NULL) {
        return false;
    }

    bool ok = false;
    lock_state();
    if (s_state == APP_STATE_RECORDING &&
        s_record_phase == APP_RECORD_PHASE_RECORDING &&
        s_pitch_frame_count < SCORE_MAX_PITCH_FRAMES) {
        s_pitch_frames[s_pitch_frame_count++] = *frame;
        ok = true;
    } else if (s_pitch_frame_count >= SCORE_MAX_PITCH_FRAMES) {
        snprintf(s_message, sizeof(s_message), "pitch frame buffer full");
    }
    unlock_state();
    return ok;
}

int app_state_get_played_count(void)
{
    /* 给网页状态和串口日志显示 played_count。 */
    int count;
    lock_state();
    count = s_played_count;
    unlock_state();
    return count;
}

void app_state_update_current_played_note(int midi,
                                          float start,
                                          float duration,
                                          float freq,
                                          float confidence)
{
    /* 以后如果做“长音合并”，可以用这个函数更新最后一个音的 duration。 */
    lock_state();
    if (s_state == APP_STATE_RECORDING && s_played_count > 0) {
        played_note_t *last = &s_played[s_played_count - 1];
        if (last->midi == midi && fabsf(last->start - start) < 0.15f) {
            last->duration = duration > 0.02f ? duration : 0.02f;
            last->freq = freq;
            last->confidence = confidence;
        }
    }
    unlock_state();
}

char *app_state_build_status_json(void)
{
    /*
     * /api/status 的核心。
     * 注意返回的是 cJSON_PrintUnformatted 分配的字符串，调用者需要 free。
     */
    lock_state();
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        unlock_state();
        return NULL;
    }

    char note_name[8];
    midi_to_note_name_buf(s_last_midi, note_name, sizeof(note_name));

    cJSON_AddStringToObject(root, "state", state_to_string(s_state));
    cJSON_AddNumberToObject(root, "target_count", s_target_count);
    cJSON_AddNumberToObject(root, "played_count", s_played_count);
    cJSON_AddNumberToObject(root, "last_freq", s_last_freq);
    cJSON_AddNumberToObject(root, "last_midi", s_last_midi);
    cJSON_AddNumberToObject(root, "freq", s_last_freq);
    cJSON_AddNumberToObject(root, "midi", s_last_midi);
    cJSON_AddNumberToObject(root, "pitch_confidence", s_last_confidence);
    cJSON_AddStringToObject(root, "last_note", note_name);
    cJSON_AddStringToObject(root, "note", note_name);
    cJSON_AddNumberToObject(root, "rms", s_last_rms);
    cJSON_AddNumberToObject(root, "noise_floor", s_noise_floor);
    cJSON_AddNumberToObject(root, "dynamic_threshold", s_dynamic_threshold);
    cJSON_AddBoolToObject(root, "noise_warning", s_noise_warning);
    cJSON_AddStringToObject(root, "record_phase", phase_to_string(s_record_phase));
    cJSON_AddNumberToObject(root, "performance_start_delay", s_performance_start_delay);
    cJSON_AddNumberToObject(root, "last_sheet_image_size", (double)s_last_sheet_image_size);
    cJSON_AddStringToObject(root, "message", s_message);
    cJSON_AddStringToObject(root, "title", s_title);
    cJSON_AddNumberToObject(root, "bpm", s_bpm);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    unlock_state();
    return json;
}

char *app_state_get_result_json(void)
{
    /* 返回最近一次评分结果的拷贝，避免调用者误改内部指针。 */
    lock_state();
    char *copy = NULL;
    if (s_last_result_json != NULL) {
        copy = dup_string(s_last_result_json);
    } else {
        copy = dup_string("{\"ok\":false,\"message\":\"no result yet\"}");
    }
    unlock_state();
    return copy;
}

char *app_state_build_ai_context_json(void)
{
    lock_state();

    int target_limit = s_target_count < AI_CONTEXT_MAX_NOTES ? s_target_count : AI_CONTEXT_MAX_NOTES;
    int played_limit = s_played_count < AI_CONTEXT_MAX_NOTES ? s_played_count : AI_CONTEXT_MAX_NOTES;
    ai_json_builder_t json = { 0 };

    ai_json_append_raw(&json, "{\"title\":");
    ai_json_append_escaped(&json, s_title);
    ai_json_appendf(&json,
                    ",\"bpm\":%d,\"target_count\":%d,\"played_count\":%d"
                    ",\"included_target_count\":%d,\"included_played_count\":%d"
                    ",\"context_truncated\":%s,\"ai_should_ignore_local_result\":true"
                    ",\"last_sheet_image_size\":%u,\"target_notes\":[",
                    s_bpm,
                    s_target_count,
                    s_played_count,
                    target_limit,
                    played_limit,
                    (s_target_count > target_limit || s_played_count > played_limit) ? "true" : "false",
                    (unsigned)s_last_sheet_image_size);

    for (int i = 0; i < target_limit; ++i) {
        if (i > 0) {
            ai_json_append_raw(&json, ",");
        }
        ai_json_appendf(&json,
                        "{\"i\":%d,\"m\":%d,\"s\":%.3f,\"d\":%.3f}",
                        i + 1,
                        s_targets[i].midi,
                        s_targets[i].start,
                        s_targets[i].duration);
    }

    ai_json_append_raw(&json, "],\"played_notes\":[");
    for (int i = 0; i < played_limit; ++i) {
        if (i > 0) {
            ai_json_append_raw(&json, ",");
        }
        ai_json_appendf(&json,
                        "{\"i\":%d,\"m\":%d,\"s\":%.3f,\"d\":%.3f,\"f\":%.1f,\"c\":%.2f",
                        i + 1,
                        s_played[i].midi,
                        s_played[i].start,
                        s_played[i].duration,
                        s_played[i].freq,
                        s_played[i].confidence);
        if (s_played[i].midi_mean > 0.0f) {
            ai_json_appendf(&json, ",\"mm\":%.2f", s_played[i].midi_mean);
        }
        if (s_played[i].midi_median > 0.0f) {
            ai_json_appendf(&json, ",\"md\":%.2f", s_played[i].midi_median);
        }
        if (s_played[i].pitch_std > 0.0f) {
            ai_json_appendf(&json, ",\"ps\":%.3f", s_played[i].pitch_std);
        }
        ai_json_append_raw(&json, "}");
    }

    ai_json_append_raw(&json,
                       "],\"field_aliases\":{\"i\":\"index\",\"m\":\"midi\",\"s\":\"start\","
                       "\"d\":\"duration\",\"f\":\"freq\",\"c\":\"confidence\","
                       "\"mm\":\"midi_mean\",\"md\":\"midi_median\",\"ps\":\"pitch_std\"}}");
    unlock_state();

    if (json.failed || json.data == NULL) {
        free(json.data);
        ESP_LOGW(TAG,
                 "failed to build ai context: target=%d played=%d cap=%u max=%u",
                 target_limit,
                 played_limit,
                 (unsigned)json.cap,
                 (unsigned)AI_CONTEXT_MAX_BYTES);
        return dup_string("{\"ok\":false,\"message\":\"failed to build ai context\"}");
    }
    ESP_LOGI(TAG,
             "ai context built: bytes=%u target=%d played=%d",
             (unsigned)json.len,
             target_limit,
             played_limit);
    return json.data;
}
