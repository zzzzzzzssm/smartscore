#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 标准乐谱和实际演奏最多各保存 1024 个音符，避免 ESP32 内存压力过大。 */
#define SCORE_MAX_TARGET_NOTES 1024
#define SCORE_MAX_PLAYED_NOTES 1024
#define SCORE_MAX_PITCH_FRAMES 1536

/* 标准乐谱中的一个音符：MIDI 音高 + 开始时间 + 持续时间。 */
typedef struct {
    int midi;
    float start;
    float duration;
} target_note_t;

/* 实际演奏中识别到的一个音符：包含频率和检测可信度，便于后续调试。 */
typedef struct {
    int midi;
    float start;
    float duration;
    float freq;
    float confidence;
    /*
     * Optional richer pitch statistics. Existing recording code may leave these
     * as 0; score_engine.c falls back to midi/confidence defaults.
     */
    float midi_mean;
    float midi_median;
    float pitch_std;
} played_note_t;

/*
 * Frame-level pitch observation used by the newer score alignment path.
 * Unlike played_note_t, this keeps short pitch samples even when they were not
 * promoted into a recorded note event by the realtime gate.
 */
typedef struct {
    float time;
    int midi;
    float freq;
    float confidence;
    float rms;
} pitch_frame_t;

/*
 * 根据标准乐谱和演奏记录生成评分结果 JSON。
 * 内部使用编辑距离做序列对齐，能处理漏识别、额外噪声和轻微错音。
 * 返回值是 malloc 出来的字符串，调用者需要 free。
 */
char *score_engine_build_result_json(const char *title,
                                     const target_note_t *target,
                                     int target_count,
                                     const played_note_t *played,
                                     int played_count,
                                     float performance_start_delay);

char *score_engine_build_result_json_with_frames(const char *title,
                                                 const target_note_t *target,
                                                 int target_count,
                                                 const played_note_t *played,
                                                 int played_count,
                                                 const pitch_frame_t *frames,
                                                 int frame_count,
                                                 float performance_start_delay);

char *score_engine_build_frame_result_json(const char *title,
                                           const target_note_t *target,
                                           int target_count,
                                           const pitch_frame_t *frames,
                                           int frame_count,
                                           const played_note_t *played,
                                           int played_count,
                                           float performance_start_delay);

#ifdef __cplusplus
}
#endif
