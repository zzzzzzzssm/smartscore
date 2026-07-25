#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "score_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 音符结果回调：用于实时更新 UI 显示 */
typedef void (*audio_session_note_cb_t)(int target_index, int expected_midi,
                                         int played_midi, float confidence,
                                         bool pitch_ok, bool rhythm_ok);

/* 评分完成回调 */
typedef void (*audio_session_score_cb_t)(const char *result_json);

/* 状态更新回调 */
typedef void (*audio_session_status_cb_t)(const char *status_json);

esp_err_t audio_session_init(void);

void audio_session_register_note_callback(audio_session_note_cb_t cb);
void audio_session_register_score_callback(audio_session_score_cb_t cb);
void audio_session_register_status_callback(audio_session_status_cb_t cb);

esp_err_t audio_session_start(void);

#ifdef __cplusplus
}
#endif
