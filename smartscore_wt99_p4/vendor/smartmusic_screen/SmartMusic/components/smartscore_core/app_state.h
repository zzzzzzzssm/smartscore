#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "score_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * app_state 模块是全工程的“共享数据中心”。
 *
 * Web API、音频任务、评分引擎都会读写这里的数据：
 * - 标准乐谱 target_notes
 * - 实际演奏 played_notes
 * - 当前 RMS / 频率 / 音符
 * - 当前录音状态
 * - 最近一次评分结果 JSON
 *
 * 这些数据会被多个 FreeRTOS 任务同时访问，所以 app_state.c 内部用互斥锁保护。
 */

/* 整个系统的大状态：是否上传乐谱、是否正在录音、是否完成评分。 */
typedef enum {
    APP_STATE_IDLE = 0,
    APP_STATE_READY,
    APP_STATE_RECORDING,
    APP_STATE_FINISHED,
} app_record_state_t;

/*
 * 录音内部阶段。
 * 当前演示版主要使用 RECORDING；保留 CALIBRATING/WAITING 是为了以后切回智能起奏。
 */
typedef enum {
    APP_RECORD_PHASE_IDLE = 0,
    APP_RECORD_PHASE_CALIBRATING,
    APP_RECORD_PHASE_WAITING,
    APP_RECORD_PHASE_RECORDING,
} app_record_phase_t;

/* 初始化互斥锁等内部资源，app_main 中启动时调用一次。 */
esp_err_t app_state_init(void);

/* 清空乐谱、演奏记录、评分结果和实时状态。 */
void app_state_clear_all(void);

/*
 * 保存网页上传的标准乐谱。
 * notes 最多保存 SCORE_MAX_TARGET_NOTES 个；如果超过，会截断并通过 truncated 告诉调用者。
 */
esp_err_t app_state_set_score(const char *title,
                              int bpm,
                              const target_note_t *notes,
                              int note_count,
                              int *stored_count,
                              bool *truncated);

/* 开始录音：清空 played_notes，把状态切换为 RECORDING。 */
esp_err_t app_state_start_recording(void);

/*
 * 强制进入正式记录阶段。
 * 用于网页“强制开始记录”按钮：环境太吵或系统一直 WAITING 时，手动把当前时刻当作演奏 0 秒。
 *
 * already_recording 为可选输出：
 * - true  表示调用前已经是 RECORDING 阶段；
 * - false 表示本次调用刚刚从 CALIBRATING/WAITING 切到 RECORDING。
 */
esp_err_t app_state_force_start_recording(bool *already_recording);

/* 停止录音，记录状态但不评分（快速，不阻塞）。 */
void app_state_stop_recording(void);

/* 对已停止的录音执行评分，生成 JSON；返回值需要调用者 free。
 * 必须在 app_state_stop_recording() 之后调用，可在独立任务中异步执行。 */
char *app_state_run_scoring(void);

/* 停止录音并同步评分（旧接口，阻塞，保留兼容）。 */
char *app_state_stop_and_score(void);

/* 给 audio_task 快速判断当前是否处于录音状态。 */
bool app_state_is_recording(void);

/* 返回从点击“开始记录”到现在经过了多少秒。 */
float app_state_record_time_sec(void);

/* 返回标准乐谱音符数量。 */
int app_state_get_target_count(void);

/* 读取标准乐谱第一个音，智能起奏模式会用到。 */
bool app_state_get_first_target_midi(int *midi);

/* 复制标准乐谱开头若干个音，audio_task 用它在预缓存中寻找真实起奏点。 */
int app_state_copy_target_prefix(target_note_t *out_notes, int max_count);

/* Copy the currently loaded target score for the offline audio session. */
int app_state_copy_targets(target_note_t *out_notes, int max_count);

/* Copy score metadata used to expand held notes into beat-level judging slots. */
void app_state_copy_score_meta(char *title, size_t title_size, int *bpm);

/* 更新网页实时显示用的音频状态。 */
void app_state_update_audio_status(float rms, float freq, int midi, float confidence);

/* 设置录音阶段，用于网页显示“正在记录”等状态。 */
void app_state_set_record_phase(app_record_phase_t phase);

/* 读取当前录音内部阶段，audio_task 用它感知网页强制开始按钮是否已生效。 */
app_record_phase_t app_state_get_record_phase(void);

/* 保留给环境校准模式，目前直接记录模式会把这些值置为 0。 */
void app_state_update_noise_status(float noise_floor, float dynamic_threshold);

/* 标记真正开始演奏的延迟；直接记录模式中延迟为 0。 */
void app_state_mark_performance_started(float performance_start_delay);

/* 保存最近一次上传乐谱图片的大小，供 /api/status 和调试日志查看。 */
void app_state_set_sheet_image_size(size_t size);

/* 追加一个识别到的演奏音符，最多 SCORE_MAX_PLAYED_NOTES 个。 */
bool app_state_add_played_note(const played_note_t *note);
bool app_state_add_pitch_frame(const pitch_frame_t *frame);

/* 返回已经记录的演奏音符数量。 */
int app_state_get_played_count(void);

/* 更新最后一个演奏音符的持续时间，保留给后续更精细的音符合并逻辑。 */
void app_state_update_current_played_note(int midi,
                                          float start,
                                          float duration,
                                          float freq,
                                          float confidence);

/* 构造 /api/status 返回的 JSON；返回值需要调用者 free。 */
char *app_state_build_status_json(void);

/* 返回最近一次评分结果 JSON 的拷贝；返回值需要调用者 free。 */
char *app_state_get_result_json(void);

/* 构造发送给 Doubao AI 评分的上下文 JSON；返回值需要调用者 free。 */
char *app_state_build_ai_context_json(void);

#ifdef __cplusplus
}
#endif
