#include "score_engine.h"

#include <math.h>
#include <stdbool.h>
#include <float.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "note_utils.h"

/*
 * 调试参数速查：
 * - 对齐太容易漏音：优先放宽 SCORE_ALIGNED_*、SCORE_TIME_GATE_EXTENSION_SEC。
 * - 对齐串音/错位：收紧 SCORE_ALIGNED_*、SCORE_FRAME_*_GATE，或关闭 frame 主路径。
 * - 短音被吞掉：调小 SCORE_MERGE_*，尤其是 MIDI_TOL 和 MAX_GAP_SEC。
 * - 开头被误裁剪：调高 SCORE_LEADING_TRIM_MIN_SCORE，或把 MAX_AUTO_APPLY 改成 0。
 * - JSON 太大/内存紧张：调小 SCORE_MAX_DETAIL_ITEMS 或 SCORE_RESULT_MAX_BUFFER_BYTES。
 */

/* DP 内部 cost 缩放。一般不用改，只影响 uint16_t DP 代价精度。 */
#define SCORE_ALIGN_SCALE 10.0f

/* 单个 target/played 匹配代价权重。当前 match_cost_value 里使用的是局部权重，保留这些宏作历史兼容。 */
#define SCORE_MATCH_PITCH_WEIGHT 0.45f
#define SCORE_MATCH_TIME_WEIGHT 0.30f
#define SCORE_MATCH_DURATION_WEIGHT 0.15f
#define SCORE_MATCH_INTERVAL_WEIGHT 0.10f

/* DP 路径代价：missing 越大越不容易判漏音，extra 越大越不容易判多余音。 */
#define SCORE_MISSING_COST 8.0f
#define SCORE_EXTRA_COST 6.0f
#define SCORE_BAD_MATCH_COST 9.0f
#define SCORE_INF_COST 6000.0f

/* 低置信度惩罚：阈值越高，越多音符会标记 low_confidence。 */
#define SCORE_LOW_CONFIDENCE_COST 3.0f
#define SCORE_LOW_CONFIDENCE_THRESHOLD 0.50f

/* 自动估计演奏速度范围。真实演奏不应超过这个倍率，否则 tempo 会被夹住。 */
#define SCORE_TEMPO_MIN 0.60f
#define SCORE_TEMPO_MAX 1.80f

/* 自动估计整体起奏偏移范围，单位秒。延迟超过这个范围会被夹住。 */
#define SCORE_START_OFFSET_MIN_SEC -2.0f
#define SCORE_START_OFFSET_MAX_SEC 2.0f
#define SCORE_TEMPO_TARGET_START_MIN 0.05f

/* 用于挑选可靠锚点估计 tempo/offset。调大更宽容，调小更严格。 */
#define SCORE_TEMPO_RELIABLE_PITCH_MAX 1.00f
#define SCORE_TEMPO_RELIABLE_TIME_MAX 2.00f

/* rough 第一遍更宽，fine 第二遍更准。对齐漏音时可略增 FINE_GATE_SCALE。 */
#define SCORE_ROUGH_GATE_SCALE 2.0f
#define SCORE_FINE_GATE_SCALE 1.0f
#define SCORE_SOFT_TIME_GATE_MIN_SEC 0.25f
#define SCORE_HARD_TIME_GATE_MIN_SEC 0.45f

/* good_count 的音准门限，单位半音。只影响“质量好”的数量，不影响完整度。 */
#define SCORE_MATCHED_PITCH_MAX_ERROR 2.50f

/* aligned_count 的门限：影响完整度 complete_score。调大更容易算“已对齐”。 */
#define SCORE_ALIGNED_PITCH_MAX_ERROR 3.00f
#define SCORE_ALIGNED_TIME_GATE_SCALE 1.20f

/* 离群过滤：音高误差超过此值（半音）的数据直接丢弃，不参与评分也不显示。 */
#define SCORE_OUTLIER_PITCH_ERROR 6.00f

/*
 * Octave and neighbor guards:
 * - YIN/autocorr occasionally reports the same pitch class one octave low/high.
 * - If a played pitch is clearly a better fit for the next target note, do not
 *   let it "steal" the current target and make the real next note look missing.
 */
#define SCORE_OCTAVE_CORRECTION_MAX_ERROR 0.80f
#define SCORE_OCTAVE_CORRECTION_PENALTY 0.30f
#define SCORE_NEIGHBOR_STEAL_MIN_ERROR 0.85f
#define SCORE_NEIGHBOR_STEAL_NEXT_MAX_ERROR 0.65f
#define SCORE_NEIGHBOR_STEAL_MARGIN 0.55f
#define SCORE_NEIGHBOR_STEAL_TIME_MARGIN_SEC 0.35f
#define SCORE_REPEAT_REUSE_PAD_SEC 0.18f

/* hard_time_gate 外额外允许的秒数。调大可容忍 start_offset/tempo 估计偏差。 */
#define SCORE_TIME_GATE_EXTENSION_SEC 0.60f

/* detail 反馈阈值：超过才提示 pitch/rhythm/duration 问题。 */
#define SCORE_PITCH_FEEDBACK_THRESHOLD 0.50f
#define SCORE_TIME_FEEDBACK_THRESHOLD 0.15f
#define SCORE_DURATION_FEEDBACK_THRESHOLD 0.25f

/* 目前总分不再直接扣这些 penalty，保留在 JSON 里作调试字段。 */
#define SCORE_EXTRA_PENALTY_PER_NOTE 2.0f
#define SCORE_MISSING_PENALTY_PER_NOTE 3.0f
#define SCORE_LOW_CONFIDENCE_PENALTY_PER_NOTE 1.5f

/* 贪心对齐重新找锚点的窗口，越大越能跨过局部错位，但更耗时。 */
#define SCORE_RESYNC_WINDOW 5

/* 每次 JSON 最多输出多少条 detail，防止 ESP32 内存压力过大。 */
#define SCORE_MAX_DETAIL_ITEMS 256
#define SCORE_RESULT_MIN_BUFFER_BYTES (32 * 1024)
#define SCORE_RESULT_MAX_BUFFER_BYTES (160 * 1024)

/*
 * details 输出上限。
 * NOTE_RECORD_INTERVAL_SEC 改成 0.05f 后，原始音高点会变多。
 * score_engine 虽然会先合并连续相同音符，但详情数量仍可能超过旧上限 128。
 * 这里提高到 256，通常够一首短曲调试使用。
 *
 * 如果 ESP32 内存不足，可以改回 128；
 * 如果仍被截断，可以改成 384，但不建议无限增大。
 */
#define SCORE_DETAIL_COMPACT_MODE 1
/*
 * 连续相同音符合并：
 * main.c 现在可以用更短的 0.10s 间隔记录音高点；
 * score_engine 在评分前把连续相同/相近的音高点合并成真正的 played_note。
 */
#define SCORE_MERGE_PLAYED_NOTES_ENABLE 1
/* 合并相邻 played_note 的音高容差，单位半音。调大更容易把滑音/邻近音合并。 */
#define SCORE_MERGE_MIDI_TOL 0.60f
/* 两个 played_note 间隔小于该值才允许合并，单位秒。调大容易吞掉短休止。 */
#define SCORE_MERGE_MAX_GAP_SEC 0.16f
/* played_note 最小时值，单位秒。调大可补偿采样太短，但会放大时值误差。 */
#define SCORE_MERGE_MIN_DURATION_SEC 0.05f
/* 合并后最多保留的 played_note 数，避免超大输入撑爆内存。 */
#define SCORE_MERGE_MAX_OUTPUT_NOTES 256

/*
 * 自动剔除开头杂音：
 * 在 played_notes 前若干个音符中寻找一段最像标准谱面开头的旋律，
 * 如果找到，就从那里开始评分，前面的误触发/杂音不参与评分。
 */
#define SCORE_LEADING_TRIM_ENABLE 1
/* 只在开头多少个 played_note 内找裁剪候选。调大更激进，可能误裁真实开头。 */
#define SCORE_LEADING_TRIM_SEARCH_NOTES 12
/* 用 target 开头多少个音评估裁剪候选。 */
#define SCORE_LEADING_TRIM_TARGET_NOTES 8
/* 裁剪候选最低分。调高更保守，调低更容易裁掉开头噪声。 */
#define SCORE_LEADING_TRIM_MIN_SCORE 6.0f
#define SCORE_LEADING_TRIM_PITCH_TOL 1.25f
#define SCORE_LEADING_TRIM_INTERVAL_TOL 1.50f
/* 候选裁剪超过这个数量时不自动裁剪，只在 JSON 里标出来。改 0 基本等于禁用自动裁剪。 */
#define SCORE_LEADING_TRIM_MAX_AUTO_APPLY 3

/*
 * Frame-level alignment path:
 * - keeps short pitch observations that never became played_note_t events;
 * - searches a small tempo/offset grid;
 * - matches each target note to the best monotonic pitch frame in its expected
 *   time window, which is less sensitive to early noise than leading trim.
 */
/* frame 主路径/补漏路径的参数。frames 噪声多时，优先提高 CONFIDENCE 或收紧 PITCH_GATE。 */
#define SCORE_FRAME_MIN_CONFIDENCE 0.20f
#define SCORE_FRAME_STRONG_CONFIDENCE 0.35f
/* frame 主对齐允许的最大音高误差，单位半音。调大更容易错配，调小更容易漏短音。 */
#define SCORE_FRAME_PITCH_GATE 2.25f
/* frame 质量反馈阈值，不影响是否匹配，只影响 result/错误计数。 */
#define SCORE_FRAME_GOOD_PITCH_ERROR 0.70f
#define SCORE_FRAME_GOOD_TIME_ERROR 0.20f
/* frame 搜索 tempo/offset 的目标音数量上限，越大越稳但越慢。 */
#define SCORE_FRAME_GRID_TARGET_LIMIT 64
/* frame 主路径 tempo 搜索范围和步长。offset/tempo 不准时可先调 STEP 更小。 */
#define SCORE_FRAME_TEMPO_MIN 0.75f
#define SCORE_FRAME_TEMPO_MAX 1.35f
#define SCORE_FRAME_TEMPO_STEP 0.05f
#define SCORE_FRAME_OFFSET_MIN_SEC -2.00f
#define SCORE_FRAME_OFFSET_MAX_SEC 2.00f
#define SCORE_FRAME_OFFSET_STEP_SEC 0.20f
#define SCORE_FRAME_OFFSET_REFINE_RANGE_SEC 0.20f
#define SCORE_FRAME_OFFSET_REFINE_STEP_SEC 0.05f
/* 未使用 frame 形成 extra 段的聚合参数。 */
#define SCORE_FRAME_EXTRA_GAP_SEC 0.24f
#define SCORE_FRAME_EXTRA_MIDI_TOL 1.00f
/* 音符级 DP missing 后的 frame 补漏窗口：[start-pad, end+pad]。 */
#define SCORE_FRAME_RECOVERY_PAD_SEC 0.25f
#define SCORE_FRAME_RECOVERY_MIN_CONFIDENCE 0.20f
#define SCORE_FRAME_RECOVERY_MIDI_TOL 1.25f


/*
 * Full edit-distance DP for 1024x1024 would need several MB of memory. Small
 * scores use DP; larger scores fall back to a greedy alignment to stay ESP32
 * friendly.
 */
#define ALIGN_DP_MAX_CELLS (260 * 260)

typedef enum {
    ALIGN_DIAG = 0,
    ALIGN_MISSING = 1,
    ALIGN_EXTRA = 2,
} align_op_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool failed;
} score_json_builder_t;

typedef struct {
    int target_idx;
    int played_idx;
    align_op_t op;
} alignment_step_t;

typedef struct {
    float tempo_scale;
    float start_offset;
    float gate_scale;
} alignment_context_t;

typedef struct {
    float raw_pitch_error;
    float pitch_error;
    float time_error;
    float duration_error_ratio;
    float interval_error;
    float match_cost;
    float adjusted_target_start;
    float adjusted_target_duration;
    float soft_time_gate;
    float hard_time_gate;
    bool blocked_by_time;
    bool octave_corrected;
    float corrected_played_midi;
} match_metrics_t;

typedef struct {
    bool matched;
    int frame_idx;
    float frame_time;
    float frame_midi;
    float raw_pitch_error;
    float pitch_error;
    float time_error;
    float match_cost;
    float confidence;
    bool octave_corrected;
    float adjusted_target_start;
    float adjusted_target_end;
} frame_match_t;

typedef struct {
    bool matched;
    int frame_idx;
    float frame_time;
    float frame_midi;
    float pitch_error;
    float time_error;
    float match_cost;
    float confidence;
    float adjusted_target_start;
    float adjusted_target_end;
    float hard_time_gate;
} frame_recovery_match_t;

static float clamp_float(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static float max_float(float a, float b)
{
    return a > b ? a : b;
}


static float played_confidence_value(const played_note_t *note);
static float played_pitch_std_value(const played_note_t *note);
static bool frame_recovery_find_match(const target_note_t *target,
                                      const pitch_frame_t *frames,
                                      int frame_count,
                                      const alignment_context_t *ctx,
                                      const bool *used_frames,
                                      frame_recovery_match_t *out);
static char *score_engine_build_result_json_internal(const char *title,
                                                     const target_note_t *target,
                                                     int target_count,
                                                     const played_note_t *played,
                                                     int played_count,
                                                     const pitch_frame_t *frames,
                                                     int frame_count,
                                                     float performance_start_delay);

static bool finite_float(float x)
{
    return isfinite(x);
}

static float safe_float(float x, float fallback)
{
    return finite_float(x) ? x : fallback;
}

static float safe_positive_float(float x, float fallback)
{
    if (!finite_float(x) || x <= 0.0f) {
        return fallback;
    }
    return x;
}

static float smooth_score_from_error(float error, float excellent, float ok, float bad)
{
    error = fabsf(safe_float(error, bad));
    if (error <= excellent) {
        return 100.0f;
    }
    if (error >= bad) {
        return 0.0f;
    }

    float x;
    if (error <= ok) {
        x = (error - excellent) / max_float(0.001f, ok - excellent);
        return 100.0f - 25.0f * x * x;
    }

    x = (error - ok) / max_float(0.001f, bad - ok);
    return 75.0f * (1.0f - x * x);
}

static float confidence_weight_value(const played_note_t *note)
{
    float c = played_confidence_value(note);
    c = clamp_float(c, 0.0f, 1.0f);
    if (c >= 0.80f) {
        return 1.0f;
    }
    if (c >= 0.50f) {
        return 0.75f + 0.25f * (c - 0.50f) / 0.30f;
    }
    return 0.50f;
}

static float adaptive_pitch_tolerance(const played_note_t *note)
{
    float stdv = played_pitch_std_value(note);
    return clamp_float(0.35f + 0.80f * stdv, 0.35f, 0.90f);
}

static int rounded_midi(float midi)
{
    if (midi < 0.0f) {
        return -1;
    }
    return (int)floorf(midi + 0.5f);
}

static bool score_json_reserve(score_json_builder_t *builder, size_t needed)
{
    if (builder == NULL || builder->failed) {
        return false;
    }
    if (needed <= builder->cap) {
        return true;
    }
    if (needed > SCORE_RESULT_MAX_BUFFER_BYTES) {
        builder->failed = true;
        return false;
    }

    size_t next_cap = builder->cap == 0 ? SCORE_RESULT_MIN_BUFFER_BYTES : builder->cap;
    while (next_cap < needed && next_cap < SCORE_RESULT_MAX_BUFFER_BYTES) {
        next_cap *= 2;
    }
    if (next_cap > SCORE_RESULT_MAX_BUFFER_BYTES) {
        next_cap = SCORE_RESULT_MAX_BUFFER_BYTES;
    }
    if (next_cap < needed) {
        builder->failed = true;
        return false;
    }

    char *next = (char *)malloc(next_cap);
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

static bool score_json_append_raw(score_json_builder_t *builder, const char *text)
{
    if (builder == NULL || text == NULL || builder->failed) {
        return false;
    }
    size_t text_len = strlen(text);
    if (!score_json_reserve(builder, builder->len + text_len + 1)) {
        return false;
    }
    memcpy(builder->data + builder->len, text, text_len + 1);
    builder->len += text_len;
    return true;
}

static bool score_json_appendf(score_json_builder_t *builder, const char *fmt, ...)
{
    if (builder == NULL || fmt == NULL || builder->failed) {
        return false;
    }
    while (1) {
        if (!score_json_reserve(builder, builder->len + 128)) {
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
        if (!score_json_reserve(builder, needed)) {
            return false;
        }
    }
}

static bool score_json_append_escaped(score_json_builder_t *builder, const char *text)
{
    if (!score_json_append_raw(builder, "\"")) {
        return false;
    }
    const char *p = text != NULL ? text : "";
    while (*p != '\0') {
        unsigned char ch = (unsigned char)*p++;
        const char *escaped = NULL;
        char unicode[7];
        switch (ch) {
        case '\\':
            escaped = "\\\\";
            break;
        case '"':
            escaped = "\\\"";
            break;
        case '\n':
            escaped = "\\n";
            break;
        case '\r':
            escaped = "\\r";
            break;
        case '\t':
            escaped = "\\t";
            break;
        default:
            if (ch < 0x20) {
                snprintf(unicode, sizeof(unicode), "\\u%04x", ch);
                escaped = unicode;
            }
            break;
        }
        if (escaped != NULL) {
            if (!score_json_append_raw(builder, escaped)) {
                return false;
            }
        } else {
            char one[2] = { (char)ch, '\0' };
            if (!score_json_append_raw(builder, one)) {
                return false;
            }
        }
    }
    return score_json_append_raw(builder, "\"");
}

static bool score_json_append_note_name(score_json_builder_t *builder,
                                        const char *key,
                                        float midi)
{
    char name[8];
    midi_to_note_name_buf(rounded_midi(midi), name, sizeof(name));
    return score_json_appendf(builder, "\"%s\":\"%s\"", key, name);
}

static float target_midi_value(const target_note_t *note)
{
    return note != NULL ? (float)note->midi : 0.0f;
}

static float target_start_time(const target_note_t *note)
{
    return note != NULL ? note->start : 0.0f;
}

static float target_duration_value(const target_note_t *note)
{
    if (note == NULL || note->duration <= 0.0f) {
        return 0.001f;
    }
    return note->duration;
}

static float played_midi_value(const played_note_t *note)
{
    if (note == NULL) {
        return 0.0f;
    }
    if (note->midi_mean > 0.0f) {
        return note->midi_mean;
    }
    if (note->midi_median > 0.0f) {
        return note->midi_median;
    }
    return (float)note->midi;
}

static float octave_aware_pitch_error(float played_midi,
                                      float target_midi,
                                      bool *octave_corrected,
                                      float *corrected_played_midi,
                                      float *raw_pitch_error)
{
    float raw_error = fabsf(played_midi - target_midi);
    float best_error = raw_error;
    float best_midi = played_midi;
    bool corrected = false;

    for (int shift = -12; shift <= 12; shift += 24) {
        float shifted_midi = played_midi + (float)shift;
        float shifted_error = fabsf(shifted_midi - target_midi);
        if (shifted_error <= SCORE_OCTAVE_CORRECTION_MAX_ERROR &&
            shifted_error + SCORE_OCTAVE_CORRECTION_PENALTY < best_error) {
            best_error = shifted_error + SCORE_OCTAVE_CORRECTION_PENALTY;
            best_midi = shifted_midi;
            corrected = true;
        }
    }

    if (octave_corrected != NULL) {
        *octave_corrected = corrected;
    }
    if (corrected_played_midi != NULL) {
        *corrected_played_midi = best_midi;
    }
    if (raw_pitch_error != NULL) {
        *raw_pitch_error = raw_error;
    }
    return best_error;
}

static float played_start_time(const played_note_t *note)
{
    return note != NULL ? note->start : 0.0f;
}

static float played_duration_value(const played_note_t *note)
{
    if (note == NULL || note->duration <= 0.0f) {
        return 0.001f;
    }
    return note->duration;
}

static float played_confidence_value(const played_note_t *note)
{
    if (note == NULL || note->confidence <= 0.0f) {
        return 1.0f;
    }
    return note->confidence;
}

static float played_pitch_std_value(const played_note_t *note)
{
    if (note == NULL || note->pitch_std <= 0.0f) {
        return 0.0f;
    }
    return note->pitch_std;
}

static bool played_notes_can_merge(const played_note_t *a,
                                   const played_note_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    float midi_a = played_midi_value(a);
    float midi_b = played_midi_value(b);

    if (fabsf(midi_a - midi_b) > SCORE_MERGE_MIDI_TOL) {
        return false;
    }

    float a_end = played_start_time(a) + played_duration_value(a);
    float gap = played_start_time(b) - a_end;

    /*
     * 允许很小的间断，因为音高检测可能中间丢一两帧。
     */
    return gap <= SCORE_MERGE_MAX_GAP_SEC;
}

static void played_note_merge_inplace(played_note_t *base,
                                      const played_note_t *next)
{
    if (base == NULL || next == NULL) {
        return;
    }

    float base_start = played_start_time(base);
    float base_end = base_start + played_duration_value(base);
    float next_end = played_start_time(next) + played_duration_value(next);

    if (next_end > base_end) {
        base->duration = next_end - base_start;
    }

    /*
     * 用高置信度的一帧作为代表音高/频率。
     */
    if (played_confidence_value(next) >= played_confidence_value(base)) {
        base->midi = next->midi;
        base->freq = next->freq;
        base->confidence = next->confidence;
        base->midi_mean = next->midi_mean;
        base->midi_median = next->midi_median;
    }

    /*
     * pitch_std 保留较大的值，表示这个合并音内部的稳定性风险。
     */
    if (next->pitch_std > base->pitch_std) {
        base->pitch_std = next->pitch_std;
    }
}

static int merge_consecutive_played_notes(const played_note_t *played,
                                          int played_count,
                                          played_note_t *out_notes,
                                          int max_out_notes)
{
#if SCORE_MERGE_PLAYED_NOTES_ENABLE
    if (played == NULL || out_notes == NULL || played_count <= 0 || max_out_notes <= 0) {
        return 0;
    }

    int out_count = 0;

    for (int i = 0; i < played_count; ++i) {
        played_note_t current = played[i];

        if (current.duration < SCORE_MERGE_MIN_DURATION_SEC) {
            current.duration = SCORE_MERGE_MIN_DURATION_SEC;
        }

        if (out_count <= 0) {
            out_notes[out_count++] = current;
            continue;
        }

        played_note_t *last = &out_notes[out_count - 1];

        if (played_notes_can_merge(last, &current)) {
            played_note_merge_inplace(last, &current);
            continue;
        }

        if (out_count < max_out_notes) {
            out_notes[out_count++] = current;
        } else {
            /*
             * 输出空间不够时，把后续相近音尽量合并到最后一个，避免越界。
             */
            if (played_notes_can_merge(last, &current)) {
                played_note_merge_inplace(last, &current);
            }
        }
    }

    return out_count;
#else
    if (played == NULL || out_notes == NULL || played_count <= 0 || max_out_notes <= 0) {
        return 0;
    }

    int n = played_count < max_out_notes ? played_count : max_out_notes;
    for (int i = 0; i < n; ++i) {
        out_notes[i] = played[i];
    }
    return n;
#endif
}


static float duration_error_ratio_value(float played_duration,
                                        float target_duration,
                                        float tempo_scale)
{
    played_duration = safe_positive_float(played_duration, 0.001f);
    target_duration = safe_positive_float(target_duration, 0.001f);
    tempo_scale = clamp_float(safe_float(tempo_scale, 1.0f), SCORE_TEMPO_MIN, SCORE_TEMPO_MAX);

    float adjusted_target_duration = max_float(0.001f, target_duration * tempo_scale);
    float ratio = fabsf(played_duration - adjusted_target_duration) / adjusted_target_duration;
    return clamp_float(ratio, 0.0f, 3.0f);
}

static float adjusted_target_start_value(const target_note_t *target,
                                         const alignment_context_t *ctx)
{
    return ctx->start_offset + target_start_time(target) * ctx->tempo_scale;
}

static float adjusted_target_duration_value(const target_note_t *target,
                                            const alignment_context_t *ctx)
{
    return max_float(0.001f, target_duration_value(target) * ctx->tempo_scale);
}

static float soft_time_gate_value(const target_note_t *target,
                                  const alignment_context_t *ctx)
{
    float dur = target_duration_value(target);
    float tempo = clamp_float(safe_float(ctx->tempo_scale, 1.0f),
                              SCORE_TEMPO_MIN,
                              SCORE_TEMPO_MAX);

    /*
     * 节奏优化版：
     * 原来的时间门限对 MP3 播放、I2S 采集和音高检测延时比较敏感。
     * 这里放宽 soft gate，使对齐更看重相对节奏，而不是某一帧的绝对起点。
     */
    float gate = 0.26f + 0.45f * dur * tempo;
    gate = clamp_float(gate, 0.32f, 1.10f);

    return gate * max_float(0.5f, ctx->gate_scale);
}

static float hard_time_gate_value(const target_note_t *target,
                                  const alignment_context_t *ctx)
{
    float dur = target_duration_value(target);
    float tempo = clamp_float(safe_float(ctx->tempo_scale, 1.0f),
                              SCORE_TEMPO_MIN,
                              SCORE_TEMPO_MAX);

    /*
     * hard gate 只用于防止跨句乱匹配，不再过早阻断。
     * 这样 played_notes 数量稍少或稍多时，DP 仍能找到合理对应。
     */
    float gate = 0.55f + 1.05f * dur * tempo;
    gate = clamp_float(gate, 0.75f, 2.20f);

    return gate * max_float(0.5f, ctx->gate_scale);
}

static float match_cost_value(const target_note_t *target,
                              int target_idx,
                              const played_note_t *played,
                              int played_idx,
                              const alignment_context_t *ctx,
                              match_metrics_t *metrics)
{
    if (target == NULL || played == NULL || ctx == NULL) {
        return SCORE_INF_COST;
    }

    float tempo = clamp_float(safe_float(ctx->tempo_scale, 1.0f),
                              SCORE_TEMPO_MIN,
                              SCORE_TEMPO_MAX);

    float played_midi = played_midi_value(played);
    float target_midi = target_midi_value(target);
    if (!finite_float(played_midi) || !finite_float(target_midi)) {
        return SCORE_INF_COST;
    }

    bool octave_corrected = false;
    float corrected_played_midi = played_midi;
    float raw_pitch_error = 0.0f;
    float pitch_error = octave_aware_pitch_error(played_midi,
                                                 target_midi,
                                                 &octave_corrected,
                                                 &corrected_played_midi,
                                                 &raw_pitch_error);
    float adjusted_target_start = adjusted_target_start_value(target, ctx);
    float played_start = played_start_time(played);
    float time_error = fabsf(played_start - adjusted_target_start);
    float soft_time_gate = soft_time_gate_value(target, ctx);
    float hard_time_gate = hard_time_gate_value(target, ctx);
    float adjusted_target_duration = adjusted_target_duration_value(target, ctx);

    float duration_error_ratio = duration_error_ratio_value(played_duration_value(played),
                                                            target_duration_value(target),
                                                            tempo);

    float interval_error = 0.0f;
    if (target_idx > 0 && played_idx > 0) {
        float target_interval = target_midi_value(target) - target_midi_value(target - 1);
        float played_interval = played_midi_value(played) -
                                played_midi_value(played - 1);
        interval_error = fabsf(target_interval - played_interval);
        interval_error = clamp_float(interval_error, 0.0f, 6.0f);
    }

    bool outside_hard_time_gate = time_error > hard_time_gate;
    bool allow_extended_time_match =
        pitch_error <= 1.25f &&
        time_error <= hard_time_gate + SCORE_TIME_GATE_EXTENSION_SEC;
    bool blocked_by_time =
        outside_hard_time_gate &&
        !allow_extended_time_match &&
        pitch_error > SCORE_ALIGNED_PITCH_MAX_ERROR &&
        time_error > hard_time_gate + (SCORE_TIME_GATE_EXTENSION_SEC * 0.50f);

    if (metrics != NULL) {
        metrics->raw_pitch_error = raw_pitch_error;
        metrics->pitch_error = pitch_error;
        metrics->time_error = time_error;
        metrics->duration_error_ratio = duration_error_ratio;
        metrics->interval_error = interval_error;
        metrics->adjusted_target_start = adjusted_target_start;
        metrics->adjusted_target_duration = adjusted_target_duration;
        metrics->soft_time_gate = soft_time_gate;
        metrics->hard_time_gate = hard_time_gate;
        metrics->blocked_by_time = blocked_by_time;
        metrics->match_cost = SCORE_INF_COST;
        metrics->octave_corrected = octave_corrected;
        metrics->corrected_played_midi = corrected_played_midi;
    }

    if (pitch_error > 4.0f) {
        return SCORE_INF_COST;
    }

    if (blocked_by_time ||
        (outside_hard_time_gate &&
         pitch_error > 1.25f &&
         time_error > hard_time_gate + SCORE_TIME_GATE_EXTENSION_SEC)) {
        return SCORE_INF_COST;
    }

    float confidence_weight = confidence_weight_value(played);
    float pitch_tol = adaptive_pitch_tolerance(played);

    float pitch_cost = clamp_float(pitch_error / pitch_tol * 2.0f, 0.0f, 10.0f);
    float time_cost = clamp_float(time_error / max_float(0.001f, soft_time_gate) * 6.0f,
                                  0.0f,
                                  10.0f);
    if (outside_hard_time_gate) {
        float over_hard = time_error - hard_time_gate;
        time_cost += clamp_float(over_hard / SCORE_TIME_GATE_EXTENSION_SEC * 4.0f,
                                 0.0f,
                                 6.0f);
    }
    float duration_cost = clamp_float(duration_error_ratio * 6.5f, 0.0f, 10.0f);
    float interval_cost = clamp_float(interval_error * 1.2f, 0.0f, 10.0f);

    float cost = 0.48f * pitch_cost +
                 0.30f * time_cost +
                 0.14f * duration_cost +
                 0.08f * interval_cost;

    if (played_confidence_value(played) < SCORE_LOW_CONFIDENCE_THRESHOLD) {
        cost += SCORE_LOW_CONFIDENCE_COST;
    }

    if (pitch_error > SCORE_ALIGNED_PITCH_MAX_ERROR &&
        time_error > hard_time_gate + (SCORE_TIME_GATE_EXTENSION_SEC * 0.50f)) {
        return SCORE_INF_COST;
    }

    cost = cost / max_float(0.50f, confidence_weight);
    cost = clamp_float(cost, 0.0f, SCORE_BAD_MATCH_COST);

    if (metrics != NULL) {
        metrics->match_cost = cost;
    }
    return cost;
}

static uint16_t scaled_cost(float cost)
{
    if (cost < 0.0f) {
        cost = 0.0f;
    }
    if (cost > 6000.0f) {
        cost = 6000.0f;
    }
    return (uint16_t)(cost * SCORE_ALIGN_SCALE + 0.5f);
}

static uint16_t add_scaled_cost(uint16_t base, uint16_t cost)
{
    uint32_t sum = (uint32_t)base + (uint32_t)cost;
    uint16_t inf = scaled_cost(SCORE_INF_COST);
    return sum > inf ? inf : (uint16_t)sum;
}

static bool midi_is_near_with_context(const target_note_t *target,
                                      const played_note_t *played,
                                      const alignment_context_t *ctx)
{
    (void)ctx;
    return fabsf(target_midi_value(target) - played_midi_value(played)) <=
           SCORE_TEMPO_RELIABLE_PITCH_MAX;
}

static bool played_pitch_should_defer_to_next_target(const target_note_t *target,
                                                     const target_note_t *next_target,
                                                     float played_midi,
                                                     float played_time,
                                                     const alignment_context_t *ctx,
                                                     float current_pitch_error)
{
    if (target == NULL || next_target == NULL || ctx == NULL) {
        return false;
    }

    float target_midi = target_midi_value(target);
    float next_midi = target_midi_value(next_target);
    if (fabsf(target_midi - next_midi) < 0.10f) {
        return false;
    }

    bool next_octave_corrected = false;
    float next_pitch_error = octave_aware_pitch_error(played_midi,
                                                      next_midi,
                                                      &next_octave_corrected,
                                                      NULL,
                                                      NULL);
    (void)next_octave_corrected;

    if (current_pitch_error < SCORE_NEIGHBOR_STEAL_MIN_ERROR ||
        next_pitch_error > SCORE_NEIGHBOR_STEAL_NEXT_MAX_ERROR ||
        current_pitch_error < next_pitch_error + SCORE_NEIGHBOR_STEAL_MARGIN) {
        return false;
    }

    float current_time_error = fabsf(played_time - adjusted_target_start_value(target, ctx));
    float next_time_error = fabsf(played_time - adjusted_target_start_value(next_target, ctx));
    float next_hard_gate = hard_time_gate_value(next_target, ctx);
    if (next_time_error > next_hard_gate * SCORE_ALIGNED_TIME_GATE_SCALE) {
        return false;
    }

    return next_time_error <= current_time_error + SCORE_NEIGHBOR_STEAL_TIME_MARGIN_SEC;
}

static float leading_trim_candidate_score(const target_note_t *target,
                                          int target_count,
                                          const played_note_t *played,
                                          int played_count,
                                          int played_start_idx)
{
    if (target == NULL || played == NULL ||
        target_count <= 0 || played_count <= 0 ||
        played_start_idx < 0 || played_start_idx >= played_count) {
        return -1000.0f;
    }

    int target_limit = target_count;
    if (target_limit > SCORE_LEADING_TRIM_TARGET_NOTES) {
        target_limit = SCORE_LEADING_TRIM_TARGET_NOTES;
    }

    int matched = 0;
    int mismatched = 0;
    float score = 0.0f;

    int last_t = -1;
    int last_p = -1;

    int p = played_start_idx;
    for (int t = 0; t < target_limit && p < played_count; ++t) {
        bool found = false;
        int local_limit = p + 4;
        if (local_limit > played_count) {
            local_limit = played_count;
        }

        for (int q = p; q < local_limit; ++q) {
            float pitch_error = fabsf(played_midi_value(&played[q]) -
                                      target_midi_value(&target[t]));

            if (pitch_error <= SCORE_LEADING_TRIM_PITCH_TOL) {
                found = true;
                matched++;

                /*
                 * 音高越准，得分越高。
                 */
                score += 1.5f - 0.4f * pitch_error;

                /*
                 * 从第二个匹配音开始，检查旋律走向/音程是否也相似。
                 * 这可以防止某个开头噪声刚好等于 C4 就被误认为起点。
                 */
                if (last_t >= 0 && last_p >= 0) {
                    float target_interval = target_midi_value(&target[t]) -
                                            target_midi_value(&target[last_t]);
                    float played_interval = played_midi_value(&played[q]) -
                                            played_midi_value(&played[last_p]);
                    float interval_error = fabsf(target_interval - played_interval);

                    if (interval_error <= SCORE_LEADING_TRIM_INTERVAL_TOL) {
                        score += 0.8f;
                    } else {
                        score -= 0.6f;
                    }
                }

                last_t = t;
                last_p = q;
                p = q + 1;
                break;
            }
        }

        if (!found) {
            mismatched++;
            score -= 0.8f;
        }
    }

    /*
     * 至少匹配到前几个音，才认为是可靠起点。
     */
    if (matched < 3) {
        return -1000.0f;
    }

    /*
     * 匹配数量越多越可靠；开头候选越靠后，稍微给一点惩罚，
     * 避免无意义地跳过太多正确音。
     */
    score += (float)matched * 0.4f;
    score -= (float)mismatched * 0.3f;
    score -= (float)played_start_idx * 0.08f;

    return score;
}

static int estimate_leading_trim_count(const target_note_t *target,
                                       int target_count,
                                       const played_note_t *played,
                                       int played_count,
                                       float *out_best_score)
{
#if SCORE_LEADING_TRIM_ENABLE
    if (out_best_score != NULL) {
        *out_best_score = 0.0f;
    }

    if (target == NULL || played == NULL || target_count <= 0 || played_count <= 0) {
        return 0;
    }

    int search_limit = played_count;
    if (search_limit > SCORE_LEADING_TRIM_SEARCH_NOTES) {
        search_limit = SCORE_LEADING_TRIM_SEARCH_NOTES;
    }

    float score0 = leading_trim_candidate_score(target,
                                                target_count,
                                                played,
                                                played_count,
                                                0);

    int best_idx = 0;
    float best_score = score0;

    for (int j = 1; j < search_limit; ++j) {
        float score = leading_trim_candidate_score(target,
                                                   target_count,
                                                   played,
                                                   played_count,
                                                   j);
        if (score > best_score) {
            best_score = score;
            best_idx = j;
        }
    }

    if (out_best_score != NULL) {
        *out_best_score = best_score;
    }

    /*
     * 只有候选起点明显优于从 played[0] 开始，才执行剔除。
     * 这样可以避免正常开头被误删。
     */
    if (best_idx > 0 &&
        best_score >= SCORE_LEADING_TRIM_MIN_SCORE &&
        (best_score - score0) >= 1.2f) {
        return best_idx;
    }

    return 0;
#else
    (void)target;
    (void)target_count;
    (void)played;
    (void)played_count;
    if (out_best_score != NULL) {
        *out_best_score = 0.0f;
    }
    return 0;
#endif
}

static bool pair_is_anchor(const target_note_t *target,
                           int target_idx,
                           const played_note_t *played,
                           int played_idx,
                           const alignment_context_t *ctx)
{
    match_metrics_t metrics = { 0 };
    float cost = match_cost_value(target, target_idx, played, played_idx, ctx, &metrics);
    return cost < SCORE_INF_COST &&
           metrics.pitch_error <= SCORE_PITCH_FEEDBACK_THRESHOLD &&
           metrics.time_error <= metrics.hard_time_gate;
}

static bool find_resync_anchor(const target_note_t *target,
                               int target_count,
                               const played_note_t *played,
                               int played_count,
                               int start_i,
                               int start_j,
                               const alignment_context_t *ctx,
                               int *anchor_i,
                               int *anchor_j)
{
    int end_i = start_i + SCORE_RESYNC_WINDOW;
    int end_j = start_j + SCORE_RESYNC_WINDOW;
    if (end_i > target_count) {
        end_i = target_count;
    }
    if (end_j > played_count) {
        end_j = played_count;
    }

    for (int i = start_i; i < end_i; ++i) {
        for (int j = start_j; j < end_j; ++j) {
            if (!pair_is_anchor(&target[i], i, &played[j], j, ctx)) {
                continue;
            }
            if ((i + 1) < target_count &&
                (j + 1) < played_count &&
                pair_is_anchor(&target[i + 1], i + 1, &played[j + 1], j + 1, ctx)) {
                if (anchor_i != NULL) {
                    *anchor_i = i;
                }
                if (anchor_j != NULL) {
                    *anchor_j = j;
                }
                return true;
            }
        }
    }
    return false;
}

static int align_notes_greedy(const target_note_t *target,
                              int target_count,
                              const played_note_t *played,
                              int played_count,
                              const alignment_context_t *ctx,
                              alignment_step_t *steps,
                              int max_steps)
{
    int i = 0;
    int j = 0;
    int count = 0;

    while ((i < target_count || j < played_count) && count < max_steps) {
        if (i >= target_count) {
            steps[count++] = (alignment_step_t){ .target_idx = -1, .played_idx = j++, .op = ALIGN_EXTRA };
            continue;
        }
        if (j >= played_count) {
            steps[count++] = (alignment_step_t){ .target_idx = i++, .played_idx = -1, .op = ALIGN_MISSING };
            continue;
        }

        match_metrics_t current_metrics = { 0 };
        float current = match_cost_value(&target[i], i, &played[j], j, ctx, &current_metrics);
        if (current < SCORE_INF_COST && midi_is_near_with_context(&target[i], &played[j], ctx)) {
            steps[count++] = (alignment_step_t){ .target_idx = i++, .played_idx = j++, .op = ALIGN_DIAG };
        } else if ((i + 1) < target_count && midi_is_near_with_context(&target[i + 1], &played[j], ctx)) {
            steps[count++] = (alignment_step_t){ .target_idx = i++, .played_idx = -1, .op = ALIGN_MISSING };
        } else if ((j + 1) < played_count && midi_is_near_with_context(&target[i], &played[j + 1], ctx)) {
            steps[count++] = (alignment_step_t){ .target_idx = -1, .played_idx = j++, .op = ALIGN_EXTRA };
        } else {
            int anchor_i = -1;
            int anchor_j = -1;
            if ((current >= SCORE_BAD_MATCH_COST || current_metrics.blocked_by_time) &&
                find_resync_anchor(target,
                                   target_count,
                                   played,
                                   played_count,
                                   i,
                                   j,
                                   ctx,
                                   &anchor_i,
                                   &anchor_j)) {
                if (anchor_i > i) {
                    steps[count++] = (alignment_step_t){ .target_idx = i++, .played_idx = -1, .op = ALIGN_MISSING };
                    continue;
                }
                if (anchor_j > j) {
                    steps[count++] = (alignment_step_t){ .target_idx = -1, .played_idx = j++, .op = ALIGN_EXTRA };
                    continue;
                }
            }

            float skip_target = (i + 1) < target_count
                                    ? match_cost_value(&target[i + 1], i + 1, &played[j], j, ctx, NULL) + SCORE_MISSING_COST
                                    : SCORE_MISSING_COST + SCORE_EXTRA_COST;
            float skip_played = (j + 1) < played_count
                                    ? match_cost_value(&target[i], i, &played[j + 1], j + 1, ctx, NULL) + SCORE_EXTRA_COST
                                    : SCORE_MISSING_COST + SCORE_EXTRA_COST;
            if (current >= SCORE_INF_COST && skip_played < skip_target) {
                steps[count++] = (alignment_step_t){ .target_idx = -1, .played_idx = j++, .op = ALIGN_EXTRA };
            } else if (current >= SCORE_INF_COST || (skip_target < current && skip_target <= skip_played)) {
                steps[count++] = (alignment_step_t){ .target_idx = i++, .played_idx = -1, .op = ALIGN_MISSING };
            } else if (skip_played < current) {
                steps[count++] = (alignment_step_t){ .target_idx = -1, .played_idx = j++, .op = ALIGN_EXTRA };
            } else {
                steps[count++] = (alignment_step_t){ .target_idx = i++, .played_idx = j++, .op = ALIGN_DIAG };
            }
        }
    }

    return count;
}

static int align_notes_by_edit_distance(const target_note_t *target,
                                        int target_count,
                                        const played_note_t *played,
                                        int played_count,
                                        const alignment_context_t *ctx,
                                        alignment_step_t *steps,
                                        int max_steps)
{
    int rows = target_count + 1;
    int cols = played_count + 1;
    int cells = rows * cols;

    if (cells > ALIGN_DP_MAX_CELLS) {
        return align_notes_greedy(target, target_count, played, played_count, ctx, steps, max_steps);
    }

    uint16_t *dp = calloc((size_t)cells, sizeof(uint16_t));
    uint8_t *op = calloc((size_t)cells, sizeof(uint8_t));
    alignment_step_t *reverse = calloc((size_t)max_steps, sizeof(alignment_step_t));
    if (dp == NULL || op == NULL || reverse == NULL) {
        free(dp);
        free(op);
        free(reverse);
        return -1;
    }

    uint16_t missing_cost = scaled_cost(SCORE_MISSING_COST);
    uint16_t extra_cost = scaled_cost(SCORE_EXTRA_COST);

    for (int i = 1; i <= target_count; ++i) {
        dp[i * cols] = add_scaled_cost(dp[(i - 1) * cols], missing_cost);
        op[i * cols] = ALIGN_MISSING;
    }
    for (int j = 1; j <= played_count; ++j) {
        dp[j] = add_scaled_cost(dp[j - 1], extra_cost);
        op[j] = ALIGN_EXTRA;
    }

    for (int i = 1; i <= target_count; ++i) {
        for (int j = 1; j <= played_count; ++j) {
            float cost = match_cost_value(&target[i - 1],
                                          i - 1,
                                          &played[j - 1],
                                          j - 1,
                                          ctx,
                                          NULL);
            uint16_t diag = add_scaled_cost(dp[(i - 1) * cols + (j - 1)],
                                            scaled_cost(cost));
            uint16_t missing = add_scaled_cost(dp[(i - 1) * cols + j], missing_cost);
            uint16_t extra = add_scaled_cost(dp[i * cols + (j - 1)], extra_cost);

            uint16_t best = diag;
            uint8_t best_op = ALIGN_DIAG;
            if (missing < best) {
                best = missing;
                best_op = ALIGN_MISSING;
            }
            if (extra < best) {
                best = extra;
                best_op = ALIGN_EXTRA;
            }

            dp[i * cols + j] = best;
            op[i * cols + j] = best_op;
        }
    }

    int i = target_count;
    int j = played_count;
    int reverse_count = 0;
    while ((i > 0 || j > 0) && reverse_count < max_steps) {
        uint8_t current = op[i * cols + j];
        if (i > 0 && j > 0 && current == ALIGN_DIAG) {
            reverse[reverse_count++] = (alignment_step_t){
                .target_idx = i - 1,
                .played_idx = j - 1,
                .op = ALIGN_DIAG,
            };
            i--;
            j--;
        } else if (i > 0 && (j == 0 || current == ALIGN_MISSING)) {
            reverse[reverse_count++] = (alignment_step_t){
                .target_idx = i - 1,
                .played_idx = -1,
                .op = ALIGN_MISSING,
            };
            i--;
        } else if (j > 0) {
            reverse[reverse_count++] = (alignment_step_t){
                .target_idx = -1,
                .played_idx = j - 1,
                .op = ALIGN_EXTRA,
            };
            j--;
        } else {
            break;
        }
    }

    int step_count = 0;
    for (int k = reverse_count - 1; k >= 0 && step_count < max_steps; --k) {
        steps[step_count++] = reverse[k];
    }

    free(dp);
    free(op);
    free(reverse);
    return step_count;
}

static void sort_float_values(float *values, int count)
{
    for (int i = 1; i < count; ++i) {
        float key = values[i];
        int j = i - 1;
        while (j >= 0 && values[j] > key) {
            values[j + 1] = values[j];
            j--;
        }
        values[j + 1] = key;
    }
}

static float median_value(float *values, int count)
{
    if (values == NULL || count <= 0) {
        return 0.0f;
    }
    sort_float_values(values, count);
    float median = values[count / 2];
    if ((count % 2) == 0) {
        median = (values[count / 2 - 1] + values[count / 2]) * 0.5f;
    }
    return median;
}

static bool timing_pair_is_reliable(const target_note_t *target,
                                    const played_note_t *played,
                                    const alignment_context_t *ctx)
{
    match_metrics_t metrics = { 0 };
    float cost = match_cost_value(target, 0, played, 0, ctx, &metrics);
    return cost < SCORE_INF_COST &&
           metrics.pitch_error <= SCORE_TEMPO_RELIABLE_PITCH_MAX &&
           metrics.time_error <= SCORE_TEMPO_RELIABLE_TIME_MAX &&
           played_confidence_value(played) >= SCORE_LOW_CONFIDENCE_THRESHOLD;
}

static void estimate_alignment_timing(const target_note_t *target,
                                      int target_count,
                                      const played_note_t *played,
                                      int played_count,
                                      const alignment_step_t *steps,
                                      int step_count,
                                      float fallback_offset,
                                      float *out_tempo_scale,
                                      float *out_start_offset)
{
    float *ratios = calloc((size_t)step_count, sizeof(float));
    float *offsets = calloc((size_t)step_count, sizeof(float));
    if (ratios == NULL || offsets == NULL) {
        free(ratios);
        free(offsets);
        if (out_tempo_scale != NULL) {
            *out_tempo_scale = 1.0f;
        }
        if (out_start_offset != NULL) {
            *out_start_offset = clamp_float(fallback_offset,
                                            SCORE_START_OFFSET_MIN_SEC,
                                            SCORE_START_OFFSET_MAX_SEC);
        }
        return;
    }

    alignment_context_t loose_ctx = {
        .tempo_scale = 1.0f,
        .start_offset = fallback_offset,
        .gate_scale = SCORE_ROUGH_GATE_SCALE,
    };

    int ratio_count = 0;
    int reliable_count = 0;
    float last_target_start = 0.0f;
    float last_played_start = 0.0f;
    bool has_last = false;
    for (int s = 0; s < step_count; ++s) {
        if (steps[s].op != ALIGN_DIAG) {
            continue;
        }
        const target_note_t *t = &target[steps[s].target_idx];
        const played_note_t *p = &played[steps[s].played_idx];
        if (!timing_pair_is_reliable(t, p, &loose_ctx)) {
            continue;
        }

        float target_start = target_start_time(t);
        float played_start = played_start_time(p);
        reliable_count++;
        if (has_last) {
            float target_delta = target_start - last_target_start;
            float played_delta = played_start - last_played_start;
            if (target_delta > SCORE_TEMPO_TARGET_START_MIN && played_delta > 0.0f) {
                float ratio = played_delta / target_delta;
                if (ratio >= SCORE_TEMPO_MIN && ratio <= SCORE_TEMPO_MAX) {
                    ratios[ratio_count++] = ratio;
                }
            }
        }
        last_target_start = target_start;
        last_played_start = played_start;
        has_last = true;
    }

    float tempo_scale = ratio_count >= 2 ? median_value(ratios, ratio_count) : 1.0f;
    tempo_scale = clamp_float(tempo_scale, SCORE_TEMPO_MIN, SCORE_TEMPO_MAX);

    int offset_count = 0;
    loose_ctx.tempo_scale = tempo_scale;
    for (int s = 0; s < step_count; ++s) {
        if (steps[s].op != ALIGN_DIAG) {
            continue;
        }
        const target_note_t *t = &target[steps[s].target_idx];
        const played_note_t *p = &played[steps[s].played_idx];
        if (!timing_pair_is_reliable(t, p, &loose_ctx)) {
            continue;
        }
        offsets[offset_count++] = played_start_time(p) - target_start_time(t) * tempo_scale;
    }

    float start_offset = reliable_count >= 2 && offset_count > 0
                             ? median_value(offsets, offset_count)
                             : fallback_offset;
    start_offset = clamp_float(start_offset, SCORE_START_OFFSET_MIN_SEC, SCORE_START_OFFSET_MAX_SEC);

    ratio_count = 0;
    loose_ctx.start_offset = start_offset;
    for (int s = 0; s < step_count; ++s) {
        if (steps[s].op != ALIGN_DIAG) {
            continue;
        }
        const target_note_t *t = &target[steps[s].target_idx];
        const played_note_t *p = &played[steps[s].played_idx];
        if (!timing_pair_is_reliable(t, p, &loose_ctx)) {
            continue;
        }
        float target_start = target_start_time(t);
        if (target_start > SCORE_TEMPO_TARGET_START_MIN) {
            float ratio = (played_start_time(p) - start_offset) / max_float(0.001f, target_start);
            if (ratio >= SCORE_TEMPO_MIN && ratio <= SCORE_TEMPO_MAX) {
                ratios[ratio_count++] = ratio;
            }
        }
    }

    if (ratio_count >= 2) {
        tempo_scale = clamp_float(median_value(ratios, ratio_count), SCORE_TEMPO_MIN, SCORE_TEMPO_MAX);
    }

    offset_count = 0;
    loose_ctx.tempo_scale = tempo_scale;
    for (int s = 0; s < step_count; ++s) {
        if (steps[s].op != ALIGN_DIAG) {
            continue;
        }
        const target_note_t *t = &target[steps[s].target_idx];
        const played_note_t *p = &played[steps[s].played_idx];
        if (!timing_pair_is_reliable(t, p, &loose_ctx)) {
            continue;
        }
        offsets[offset_count++] = played_start_time(p) - target_start_time(t) * tempo_scale;
    }
    if (reliable_count >= 2 && offset_count > 0) {
        start_offset = median_value(offsets, offset_count);
    }
    start_offset = clamp_float(start_offset, SCORE_START_OFFSET_MIN_SEC, SCORE_START_OFFSET_MAX_SEC);

    free(ratios);
    free(offsets);

    if (target_count <= 0 || played_count <= 0) {
        start_offset = 0.0f;
        tempo_scale = 1.0f;
    }
    if (out_tempo_scale != NULL) {
        *out_tempo_scale = tempo_scale;
    }
    if (out_start_offset != NULL) {
        *out_start_offset = start_offset;
    }
}

static float score_pitch_error(float error)
{
    return smooth_score_from_error(error, 0.15f, 0.70f, 2.80f);
}

static float score_rhythm_error(float time_error)
{
    /*
     * 节奏评分（绝对时间误差）：
     * 单位：秒。
     * MP3 播放、I2S 采集、YIN/自相关检测都会带来几十到几百毫秒延时；
     * 适当放宽阈值使评分更符合实际演奏体验。
     *
     * 设计目标：
     * - 0.15s 内：基本满分；
     * - 0.30s 内：仍认为节奏较好；
     * - 0.65s 内：中等偏好；
     * - 1.00s 内：保留一定分数；
     * - 大于 1.50s：认为明显错位。
     */
    return smooth_score_from_error(time_error, 0.15f, 0.55f, 1.35f);
}

/*
 * 相对节奏评分：
 * 不再只看某一个音的绝对开始时间，而是看相邻两个已匹配音符之间的时间间隔。
 *
 * 这样可以抵消 MP3 播放、扬声器、麦克风、I2S 缓冲、音高检测带来的固定延迟。
 * 对于“原音频播放给麦克风听”的场景，这比绝对时间误差更合理。
 */
static float score_rhythm_interval_error(float interval_error)
{
    /*
     * interval_error 单位：秒。
     * 0.08s 内认为非常准；
     * 0.18s 内认为比较准；
     * 0.45s 仍保留一定分数；
     * 1.00s 以上基本认为节奏间隔明显错误。
     */
    return smooth_score_from_error(interval_error, 0.08f, 0.30f, 1.00f);
}

static float rhythm_interval_error_value(float current_target_start,
                                         float previous_target_start,
                                         float current_played_start,
                                         float previous_played_start,
                                         float tempo_scale)
{
    float target_delta = current_target_start - previous_target_start;
    float played_delta = current_played_start - previous_played_start;

    if (target_delta < 0.001f || played_delta < 0.001f) {
        return 0.0f;
    }

    tempo_scale = clamp_float(safe_float(tempo_scale, 1.0f),
                              SCORE_TEMPO_MIN,
                              SCORE_TEMPO_MAX);

    float expected_delta = target_delta * tempo_scale;
    return fabsf(played_delta - expected_delta);
}


static float score_duration_error(float error_ratio)
{
    return smooth_score_from_error(error_ratio, 0.10f, 0.30f, 0.80f);
}

static float score_stability(float pitch_std)
{
    pitch_std = safe_float(pitch_std, 0.50f);
    return smooth_score_from_error(pitch_std, 0.05f, 0.22f, 0.60f);
}

static const char *detail_result(float pitch_error,
                                 float time_error,
                                 float duration_error_ratio)
{
    (void)duration_error_ratio;

    bool pitch_bad = pitch_error > SCORE_PITCH_FEEDBACK_THRESHOLD;
    bool rhythm_bad = time_error > SCORE_TIME_FEEDBACK_THRESHOLD;

    if (!pitch_bad && !rhythm_bad) {
        return "correct";
    }
    if (pitch_bad && !rhythm_bad) {
        return "pitch_error";
    }
    if (!pitch_bad && rhythm_bad) {
        return "rhythm_error";
    }
    return "pitch_and_rhythm_error";
}

static char *build_compact_result_json(const char *title,
                                       int target_count,
                                       int played_count,
                                       int raw_played_count,
                                       int merged_played_count,
                                       int scored_played_count,
                                       int pitch_frame_count,
                                       int aligned_count,
                                       int good_count,
                                       int frame_recovered_count,
                                       int leading_trim_count,
                                       float leading_trim_score,
                                       int ignored_played_count,
                                       float pitch_score,
                                       float rhythm_score,
                                       float duration_score,
                                       float stability_score,
                                       float complete_score,
                                       float total_score,
                                       float tempo_scale,
                                       float start_offset,
                                       float performance_start_delay,
                                       int extra_count,
                                       int missing_count,
                                       int low_confidence_count,
                                       int pitch_error_count,
                                       int rhythm_error_count,
                                       int duration_error_count,
                                       float extra_penalty,
                                       float missing_penalty,
                                       float low_confidence_penalty)
{
    (void)title;
    const char *safe_title = "untitled";
    int len = snprintf(NULL,
                       0,
                       "{\"ok\":true,\"title\":\"%s\",\"target_count\":%d,\"played_count\":%d,"
                       "\"raw_played_count\":%d,\"merged_played_count\":%d,"
                       "\"scored_played_count\":%d,\"pitch_frame_count\":%d,"
                       "\"leading_trim_count\":%d,\"leading_trim_score\":%.3f,"
                       "\"ignored_played_count\":%d,\"matched_count\":%d,"
                       "\"aligned_count\":%d,\"good_count\":%d,\"frame_recovered_count\":%d,"
                       "\"pitch_score\":%.1f,\"rhythm_score\":%.1f,"
                       "\"duration_score\":%.1f,\"stability_score\":%.1f,\"complete_score\":%.1f,"
                       "\"total_score\":%.1f,\"alignment_method\":\"edit_distance_offset_tempo_gate_compact\","
                       "\"tempo_scale\":%.4f,\"start_offset\":%.4f,\"performance_start_delay\":%.4f,"
                       "\"extra_count\":%d,\"missing_count\":%d,\"low_confidence_count\":%d,"
                       "\"pitch_error_count\":%d,\"rhythm_error_count\":%d,\"duration_error_count\":%d,"
                       "\"extra_penalty\":%.1f,\"missing_penalty\":%.1f,\"low_confidence_penalty\":%.1f,"
                       "\"details_truncated\":true,\"details\":[],"
                       "\"summary\":\"result generated successfully; details were truncated to reduce memory usage\"}",
                       safe_title,
                       target_count,
                       played_count,
                       raw_played_count,
                       merged_played_count,
                       scored_played_count,
                       pitch_frame_count,
                       leading_trim_count,
                       leading_trim_score,
                       ignored_played_count,
                       aligned_count,
                       aligned_count,
                       good_count,
                       frame_recovered_count,
                       pitch_score,
                       rhythm_score,
                       duration_score,
                       stability_score,
                       complete_score,
                       total_score,
                       tempo_scale,
                       start_offset,
                       performance_start_delay,
                       extra_count,
                       missing_count,
                       low_confidence_count,
                       pitch_error_count,
                       rhythm_error_count,
                       duration_error_count,
                       extra_penalty,
                       missing_penalty,
                       low_confidence_penalty);
    if (len <= 0) {
        return NULL;
    }
    char *json = malloc((size_t)len + 1);
    if (json == NULL) {
        return NULL;
    }
    snprintf(json,
             (size_t)len + 1,
             "{\"ok\":true,\"title\":\"%s\",\"target_count\":%d,\"played_count\":%d,"
             "\"raw_played_count\":%d,\"merged_played_count\":%d,"
             "\"scored_played_count\":%d,\"pitch_frame_count\":%d,"
             "\"leading_trim_count\":%d,\"leading_trim_score\":%.3f,"
             "\"ignored_played_count\":%d,\"matched_count\":%d,"
             "\"aligned_count\":%d,\"good_count\":%d,\"frame_recovered_count\":%d,"
             "\"pitch_score\":%.1f,\"rhythm_score\":%.1f,"
             "\"duration_score\":%.1f,\"stability_score\":%.1f,\"complete_score\":%.1f,"
             "\"total_score\":%.1f,\"alignment_method\":\"edit_distance_offset_tempo_gate_compact\","
             "\"tempo_scale\":%.4f,\"start_offset\":%.4f,\"performance_start_delay\":%.4f,"
             "\"extra_count\":%d,\"missing_count\":%d,\"low_confidence_count\":%d,"
             "\"pitch_error_count\":%d,\"rhythm_error_count\":%d,\"duration_error_count\":%d,"
             "\"extra_penalty\":%.1f,\"missing_penalty\":%.1f,\"low_confidence_penalty\":%.1f,"
             "\"details_truncated\":true,\"details\":[],"
             "\"summary\":\"result generated successfully; details were truncated to reduce memory usage\"}",
             safe_title,
             target_count,
             played_count,
             raw_played_count,
             merged_played_count,
             scored_played_count,
             pitch_frame_count,
             leading_trim_count,
             leading_trim_score,
             ignored_played_count,
             aligned_count,
             aligned_count,
             good_count,
             frame_recovered_count,
             pitch_score,
             rhythm_score,
             duration_score,
             stability_score,
             complete_score,
             total_score,
             tempo_scale,
             start_offset,
             performance_start_delay,
             extra_count,
             missing_count,
             low_confidence_count,
             pitch_error_count,
             rhythm_error_count,
             duration_error_count,
             extra_penalty,
             missing_penalty,
             low_confidence_penalty);
    return json;
}

static char *score_engine_build_result_json_internal(const char *title,
                                                     const target_note_t *target,
                                                     int target_count,
                                                     const played_note_t *played,
                                                     int played_count,
                                                     const pitch_frame_t *frames,
                                                     int frame_count,
                                                     float performance_start_delay)
{
    if (target_count < 0) {
        target_count = 0;
    }
    if (played_count < 0) {
        played_count = 0;
    }
    if (frame_count < 0) {
        frame_count = 0;
    }
    if (target_count > 0 && target == NULL) {
        target_count = 0;
    }
    if (played_count > 0 && played == NULL) {
        played_count = 0;
    }
    if (frame_count > 0 && frames == NULL) {
        frame_count = 0;
    }

    /*
     * 先把 main.c 以 0.10s 间隔记录出来的密集 played_notes 合并成真正音符。
     * 例如标准 C4 持续 0.50s，main 可能记录为：
     *   C4@0.00, C4@0.10, C4@0.20, C4@0.30, C4@0.40
     * 这里会合并成：
     *   C4 start=0.00 duration≈0.50
     */
    played_note_t *merged_played = NULL;
    int merged_played_count = played_count;

    if (played_count > 0) {
        int merge_capacity = played_count;
        if (merge_capacity < 1) {
            merge_capacity = 1;
        }
        if (merge_capacity > SCORE_MERGE_MAX_OUTPUT_NOTES) {
            merge_capacity = SCORE_MERGE_MAX_OUTPUT_NOTES;
        }

        merged_played = calloc((size_t)merge_capacity, sizeof(played_note_t));
        if (merged_played != NULL) {
            merged_played_count = merge_consecutive_played_notes(played,
                                                                 played_count,
                                                                 merged_played,
                                                                 merge_capacity);
            if (merged_played_count <= 0) {
                free(merged_played);
                merged_played = NULL;
                merged_played_count = played_count;
            }
        }
    }

    const played_note_t *input_played = merged_played != NULL ? merged_played : played;
    int input_played_count = merged_played != NULL ? merged_played_count : played_count;

    int max_steps = target_count + input_played_count;
    if (max_steps <= 0) {
        max_steps = 1;
    }

    alignment_step_t *rough_steps = calloc((size_t)max_steps, sizeof(alignment_step_t));
    alignment_step_t *steps = calloc((size_t)max_steps, sizeof(alignment_step_t));
    if (rough_steps == NULL || steps == NULL) {
        free(rough_steps);
        free(steps);
        free(merged_played);
        return NULL;
    }

    float fallback_start_offset = 0.0f;
    if (target_count > 0 && input_played_count > 0) {
        fallback_start_offset = played_start_time(&input_played[0]) - target_start_time(&target[0]);
    }
    fallback_start_offset = clamp_float(fallback_start_offset,
                                        SCORE_START_OFFSET_MIN_SEC,
                                        SCORE_START_OFFSET_MAX_SEC);
    /*
     * 自动剔除开头杂音/误触发音符。
     * 典型场景：
     * - 用户点了开始记录，但还没开始放歌；
     * - 麦克风先检测到几个杂音或误判音高；
     * - 这些音符如果进入 DP，会干扰 start_offset、tempo_scale 和 rhythm_score。
     *
     * 这里会在 played_notes 前若干个音符中寻找与 target 开头最像的旋律片段，
     * 并从该片段开始评分。
     */
    float leading_trim_score = 0.0f;
    int leading_trim_candidate_count = estimate_leading_trim_count(target,
                                                                   target_count,
                                                                   input_played,
                                                                   input_played_count,
                                                                   &leading_trim_score);
    int leading_trim_count = leading_trim_candidate_count;
    bool leading_trim_suppressed = false;
    if (leading_trim_count > SCORE_LEADING_TRIM_MAX_AUTO_APPLY) {
        leading_trim_count = 0;
        leading_trim_suppressed = true;
    }
    const played_note_t *score_played = input_played;
    int score_played_count = input_played_count;
    int ignored_played_count = 0;

    if (leading_trim_count > 0 && leading_trim_count < input_played_count) {
        score_played = input_played + leading_trim_count;
        score_played_count = input_played_count - leading_trim_count;
        ignored_played_count = leading_trim_count;

        if (target_count > 0 && score_played_count > 0) {
            fallback_start_offset = played_start_time(&score_played[0]) -
                                    target_start_time(&target[0]);
            fallback_start_offset = clamp_float(fallback_start_offset,
                                                SCORE_START_OFFSET_MIN_SEC,
                                                SCORE_START_OFFSET_MAX_SEC);
        }
    }
    alignment_context_t rough_ctx = {
        .tempo_scale = 1.0f,
        .start_offset = fallback_start_offset,
        .gate_scale = SCORE_ROUGH_GATE_SCALE,
    };

    int rough_count = align_notes_by_edit_distance(target,
                                                   target_count,
                                                   score_played,
                                                   score_played_count,
                                                   &rough_ctx,
                                                   rough_steps,
                                                   max_steps);
    if (rough_count < 0) {
        free(rough_steps);
        free(steps);
        free(merged_played);
        return NULL;
    }

    float tempo_scale = 1.0f;
    float start_offset = fallback_start_offset;
    estimate_alignment_timing(target,
                              target_count,
                              score_played,
                              score_played_count,
                              rough_steps,
                              rough_count,
                              fallback_start_offset,
                              &tempo_scale,
                              &start_offset);

    alignment_context_t fine_ctx = {
        .tempo_scale = tempo_scale,
        .start_offset = start_offset,
        .gate_scale = SCORE_FINE_GATE_SCALE,
    };

    int step_count = align_notes_by_edit_distance(target,
                                                  target_count,
                                                  score_played,
                                                  score_played_count,
                                                  &fine_ctx,
                                                  steps,
                                                  max_steps);
    free(rough_steps);
    if (step_count < 0) {
        free(steps);
        free(merged_played);
        return NULL;
    }

    bool *used_recovery_frames = NULL;
    if (frame_count > 0) {
        used_recovery_frames = calloc((size_t)frame_count, sizeof(bool));
        if (used_recovery_frames == NULL) {
            frames = NULL;
            frame_count = 0;
        }
    }

    float pitch_sum = 0.0f;
    float rhythm_sum = 0.0f;
    float duration_sum = 0.0f;
    float stability_sum = 0.0f;
    int duration_count = 0;
    int stability_count = 0;
    int aligned_count = 0;
    int good_count = 0;
    int frame_recovered_count = 0;
    float last_matched_target_start = 0.0f;
    float last_matched_played_start = 0.0f;
    bool has_last_matched_for_rhythm = false;
    int pitch_error_count = 0;
    int rhythm_error_count = 0;
    int duration_error_count = 0;
    int missing_count = 0;
    int extra_count = 0;
    int low_confidence_count = 0;
    int detail_count = 0;
    bool details_truncated = false;
    score_json_builder_t details_json = { 0 };

    for (int s = 0; s < step_count; ++s) {
        if (steps[s].op == ALIGN_EXTRA) {
            int pi = steps[s].played_idx;
            float played_midi = played_midi_value(&score_played[pi]);
            extra_count++;

            if (detail_count < SCORE_MAX_DETAIL_ITEMS && !details_json.failed) {
                if (detail_count > 0) {
                    score_json_append_raw(&details_json, ",");
                }
                score_json_appendf(&details_json,
                                   "{\"index\":%d,\"ref_index\":-1,\"played_index\":%d,"
                                   "\"is_missing\":false,\"is_extra\":true,"
                                   "\"target_midi\":-1,\"target_note\":\"-\","
                                   "\"played_midi\":%.2f,",
                                   s + 1,
                                   pi + 1 + leading_trim_count,
                                   played_midi);
                score_json_append_note_name(&details_json, "played_note", played_midi);
                score_json_appendf(&details_json,
                                   ",\"target_start\":-1,\"adjusted_target_start\":-1,"
                                   "\"played_start\":%.3f,\"time_error\":0,"
                                   "\"pitch_error\":0,\"duration_error\":0,"
                                   "\"match_cost\":%.2f,\"hard_time_gate\":0,"
                                   "\"result\":\"extra\",\"feedback_text\":\"extra note\"}",
                                   played_start_time(&score_played[pi]),
                                   SCORE_EXTRA_COST);
                if (!details_json.failed) {
                    detail_count++;
                }
            } else {
                details_truncated = true;
            }
            continue;
        }

        int ti = steps[s].target_idx;
        const target_note_t *t = &target[ti];
        float target_midi = target_midi_value(t);
        float adjusted_target_start = adjusted_target_start_value(t, &fine_ctx);
        float adjusted_target_duration = adjusted_target_duration_value(t, &fine_ctx);

        if (steps[s].op == ALIGN_MISSING) {
            frame_recovery_match_t recovery = { 0 };
            bool recovered_by_frame =
                frame_recovery_find_match(t,
                                          frames,
                                          frame_count,
                                          &fine_ctx,
                                          used_recovery_frames,
                                          &recovery);

            if (recovered_by_frame) {
                if (used_recovery_frames != NULL &&
                    recovery.frame_idx >= 0 &&
                    recovery.frame_idx < frame_count) {
                    used_recovery_frames[recovery.frame_idx] = true;
                }

                aligned_count++;
                frame_recovered_count++;
                pitch_sum += score_pitch_error(recovery.pitch_error);
                rhythm_sum += score_rhythm_error(recovery.time_error);
                last_matched_target_start = target_start_time(t);
                last_matched_played_start = recovery.frame_time;
                has_last_matched_for_rhythm = true;

                bool low_confidence = recovery.confidence < SCORE_LOW_CONFIDENCE_THRESHOLD;
                if (recovery.pitch_error > SCORE_PITCH_FEEDBACK_THRESHOLD) {
                    pitch_error_count++;
                }
                if (recovery.time_error > SCORE_TIME_FEEDBACK_THRESHOLD) {
                    rhythm_error_count++;
                }
                if (low_confidence) {
                    low_confidence_count++;
                }

                char feedback_msg[96] = "";
                if (recovery.pitch_error > SCORE_PITCH_FEEDBACK_THRESHOLD) {
                    snprintf(feedback_msg,
                             sizeof(feedback_msg),
                             recovery.frame_midi > target_midi ? "note %d high (frame recovered)" :
                                                                 "note %d low (frame recovered)",
                             ti + 1);
                } else if (recovery.time_error > SCORE_TIME_FEEDBACK_THRESHOLD) {
                    snprintf(feedback_msg,
                             sizeof(feedback_msg),
                             "note %d timing offset (frame recovered)",
                             ti + 1);
                } else if (low_confidence) {
                    snprintf(feedback_msg,
                             sizeof(feedback_msg),
                             "note %d low confidence (frame recovered)",
                             ti + 1);
                }

                if (detail_count < SCORE_MAX_DETAIL_ITEMS && !details_json.failed) {
                    if (detail_count > 0) {
                        score_json_append_raw(&details_json, ",");
                    }
                    score_json_appendf(&details_json,
                                       "{\"index\":%d,\"ref_index\":%d,\"played_index\":%d,"
                                       "\"is_missing\":false,\"is_extra\":false,"
                                       "\"matched_by_frame\":true,"
                                       "\"target_midi\":%.2f,",
                                       ti + 1,
                                       ti + 1,
                                       recovery.frame_idx + 1,
                                       target_midi);
                    score_json_append_note_name(&details_json, "target_note", target_midi);
                    score_json_appendf(&details_json,
                                       ",\"target_start\":%.3f,\"adjusted_target_start\":%.3f,"
                                       "\"adjusted_target_end\":%.3f,\"played_midi\":%.2f,",
                                       target_start_time(t),
                                       recovery.adjusted_target_start,
                                       recovery.adjusted_target_end,
                                       recovery.frame_midi);
                    score_json_append_note_name(&details_json, "played_note", recovery.frame_midi);
                    score_json_appendf(&details_json,
                                       ",\"played_start\":%.3f,\"played_duration\":0.000,"
                                       "\"time_error\":%.3f,\"pitch_error\":%.3f,"
                                       "\"duration_error\":0,\"match_cost\":%.2f,"
                                       "\"hard_time_gate\":%.3f,\"confidence\":%.2f,"
                                       "\"low_confidence\":%s,"
                                       "\"result\":\"frame_recovered\",\"feedback_text\":",
                                       recovery.frame_time,
                                       recovery.time_error,
                                       recovery.pitch_error,
                                       recovery.match_cost,
                                       recovery.hard_time_gate,
                                       recovery.confidence,
                                       low_confidence ? "true" : "false");
                    score_json_append_escaped(&details_json, feedback_msg);
                    score_json_append_raw(&details_json, "}");
                    if (!details_json.failed) {
                        detail_count++;
                    }
                } else {
                    details_truncated = true;
                }
                continue;
            }

            missing_count++;
            char feedback_msg[64];
            snprintf(feedback_msg, sizeof(feedback_msg), "note %d missing", ti + 1);
            if (detail_count < SCORE_MAX_DETAIL_ITEMS && !details_json.failed) {
                if (detail_count > 0) {
                    score_json_append_raw(&details_json, ",");
                }
                score_json_appendf(&details_json,
                                   "{\"index\":%d,\"ref_index\":%d,\"played_index\":-1,"
                                   "\"is_missing\":true,\"is_extra\":false,"
                                   "\"target_midi\":%.2f,",
                                   ti + 1,
                                   ti + 1,
                                   target_midi);
                score_json_append_note_name(&details_json, "target_note", target_midi);
                score_json_appendf(&details_json,
                                   ",\"target_start\":%.3f,\"adjusted_target_start\":%.3f,"
                                   "\"played_midi\":-1,"
                                   "\"played_note\":\"-\",\"played_start\":-1,"
                                   "\"time_error\":0,\"pitch_error\":0,\"duration_error\":0,"
                                   "\"match_cost\":%.2f,\"hard_time_gate\":%.3f,"
                                   "\"result\":\"missing\",\"feedback_text\":",
                                   target_start_time(t),
                                   adjusted_target_start,
                                   SCORE_MISSING_COST,
                                   hard_time_gate_value(t, &fine_ctx));
                score_json_append_escaped(&details_json, feedback_msg);
                score_json_append_raw(&details_json, "}");
                if (!details_json.failed) {
                    detail_count++;
                }
            } else {
                details_truncated = true;
            }
            continue;
        }

        int pi = steps[s].played_idx;
        const played_note_t *p = &score_played[pi];
        float played_midi = played_midi_value(p);
        float played_start = played_start_time(p);
        match_metrics_t metrics = { 0 };
        float match_cost = match_cost_value(t, ti, p, pi, &fine_ctx, &metrics);
        float pitch_error = metrics.pitch_error;
        float time_error = metrics.time_error;
        float duration_error_ratio = metrics.duration_error_ratio;
        bool is_outlier = pitch_error > SCORE_OUTLIER_PITCH_ERROR;

        float note_pitch_score = score_pitch_error(pitch_error);
        float note_rhythm_score = score_rhythm_error(time_error);

        /*
         * 节奏评分优化：
         * 第一个匹配音没有前一个音，只能暂时使用绝对时间误差；
         * 从第二个匹配音开始，使用“相邻匹配音符时间间隔误差”。
         */
        if (has_last_matched_for_rhythm) {
            float rhythm_interval_error = rhythm_interval_error_value(
                target_start_time(t),
                last_matched_target_start,
                played_start,
                last_matched_played_start,
                fine_ctx.tempo_scale
            );
            note_rhythm_score = score_rhythm_interval_error(rhythm_interval_error);
        }
        float note_duration_score = score_duration_error(duration_error_ratio);
        float note_stability_score = score_stability(played_pitch_std_value(p));
        bool low_confidence = played_confidence_value(p) < SCORE_LOW_CONFIDENCE_THRESHOLD;
        bool aligned = match_cost < SCORE_INF_COST &&
                       pitch_error <= SCORE_ALIGNED_PITCH_MAX_ERROR &&
                       time_error <= metrics.hard_time_gate * SCORE_ALIGNED_TIME_GATE_SCALE;
        bool good_match = aligned &&
                          pitch_error <= SCORE_MATCHED_PITCH_MAX_ERROR &&
                          time_error <= metrics.hard_time_gate &&
                          duration_error_ratio <= 1.20f;

        if (aligned) {
            pitch_sum += note_pitch_score;
            rhythm_sum += note_rhythm_score;
            duration_sum += note_duration_score;
            duration_count++;
            stability_sum += note_stability_score;
            stability_count++;
            aligned_count++;
            if (good_match) {
                good_count++;
            }
        }

        /*
         * 只有匹配成功的音符才参与下一次相对节奏间隔计算。
         * extra/missing 不直接拉低 rhythm_score，避免重复惩罚。
         */
        if (aligned) {
            last_matched_target_start = target_start_time(t);
            last_matched_played_start = played_start;
            has_last_matched_for_rhythm = true;
        }
        if (pitch_error > SCORE_PITCH_FEEDBACK_THRESHOLD) {
            pitch_error_count++;
        }
        if (time_error > SCORE_TIME_FEEDBACK_THRESHOLD) {
            rhythm_error_count++;
        }
        if (duration_error_ratio > SCORE_DURATION_FEEDBACK_THRESHOLD) {
            duration_error_count++;
        }
        if (low_confidence) {
            low_confidence_count++;
        }

        char feedback_msg[96] = "";
        if (pitch_error > SCORE_PITCH_FEEDBACK_THRESHOLD) {
            snprintf(feedback_msg,
                     sizeof(feedback_msg),
                     played_midi > target_midi ? "note %d high" : "note %d low",
                     ti + 1);
        }
        if (time_error > SCORE_TIME_FEEDBACK_THRESHOLD) {
            if (feedback_msg[0] == '\0') {
                snprintf(feedback_msg,
                         sizeof(feedback_msg),
                         played_start > adjusted_target_start ? "note %d late" : "note %d early",
                         ti + 1);
            }
        }
        if (duration_error_ratio > SCORE_DURATION_FEEDBACK_THRESHOLD) {
            if (feedback_msg[0] == '\0') {
                snprintf(feedback_msg,
                         sizeof(feedback_msg),
                         played_duration_value(p) < adjusted_target_duration ? "note %d short" : "note %d long",
                         ti + 1);
            }
        }
        if (low_confidence && feedback_msg[0] == '\0') {
            snprintf(feedback_msg, sizeof(feedback_msg), "note %d low confidence", ti + 1);
        }

        if (detail_count < SCORE_MAX_DETAIL_ITEMS && !details_json.failed) {
            if (detail_count > 0) {
                score_json_append_raw(&details_json, ",");
            }
            score_json_appendf(&details_json,
                               "{\"index\":%d,\"ref_index\":%d,\"played_index\":%d,"
                               "\"is_missing\":false,\"is_extra\":false,"
                               "\"is_outlier\":%s,"
                               "\"target_midi\":%.2f,",
                               ti + 1,
                               ti + 1,
                               pi + 1 + leading_trim_count,
                               is_outlier ? "true" : "false",
                               target_midi);
            score_json_append_note_name(&details_json, "target_note", target_midi);
            score_json_appendf(&details_json,
                               ",\"target_start\":%.3f,\"adjusted_target_start\":%.3f,"
                               "\"played_midi\":%.2f,",
                               target_start_time(t),
                               adjusted_target_start,
                               played_midi);
            score_json_append_note_name(&details_json, "played_note", played_midi);
            const char *result = aligned ? detail_result(pitch_error,
                                                         time_error,
                                                         duration_error_ratio) :
                                           "not_aligned";
            score_json_appendf(&details_json,
                               ",\"played_start\":%.3f,\"played_duration\":%.3f,"
                               "\"time_error\":%.3f,\"pitch_error\":%.3f,"
                               "\"duration_error\":%.3f,\"interval_error\":%.3f,"
                               "\"match_cost\":%.2f,\"hard_time_gate\":%.3f,"
                               "\"aligned\":%s,\"good\":%s,"
                               "\"low_confidence\":%s,\"result\":\"%s\",\"feedback_text\":",
                               played_start,
                               played_duration_value(p),
                               time_error,
                               pitch_error,
                               duration_error_ratio,
                               metrics.interval_error,
                               match_cost,
                               metrics.hard_time_gate,
                               aligned ? "true" : "false",
                               good_match ? "true" : "false",
                               low_confidence ? "true" : "false",
                               result);
            score_json_append_escaped(&details_json, feedback_msg);
            score_json_append_raw(&details_json, "}");
            if (!details_json.failed) {
                detail_count++;
            }
        } else {
            details_truncated = true;
        }
    }

    float pitch_score = 0.0f;
    float rhythm_score = 0.0f;
    float duration_score = 0.0f;
    float stability_score = 100.0f;
    float complete_score = 0.0f;
    if (aligned_count > 0) {
        /*
         * pitch_score / rhythm_score 表示已成功匹配音符的质量；
         * complete_score 单独表示完整度，避免漏音被重复惩罚。
         */
        pitch_score = clamp_float(pitch_sum / (float)aligned_count, 0.0f, 100.0f);
        rhythm_score = clamp_float(rhythm_sum / (float)aligned_count, 0.0f, 100.0f);
    }
    if (target_count > 0) {
        complete_score = clamp_float((float)aligned_count * 100.0f / (float)target_count, 0.0f, 100.0f);
    }

    /*
     * duration_score / stability_score 不参与总分，只作为调试字段保留。
     */
    if (duration_count > 0) {
        duration_score = clamp_float(duration_sum / (float)duration_count, 0.0f, 100.0f);
    }
    if (stability_count > 0) {
        stability_score = clamp_float(stability_sum / (float)stability_count, 0.0f, 100.0f);
    }
    /*
     * 最终评分公式：
     * 只使用音准、节奏、完整度三个维度。
     * 时值分、稳定性分、extra/missing/low confidence 只作为调试信息保留，
     * 不再参与总分计算。
     */
    float extra_penalty = 0.0f;
    float missing_penalty = 0.0f;
    float low_confidence_penalty = 0.0f;

    float total_score = 0.55f * pitch_score +
                        0.35f * rhythm_score +
                        0.10f * complete_score;
    total_score = clamp_float(total_score, 0.0f, 100.0f);

    int matched_count = aligned_count;
    char summary[360];
    snprintf(summary,
             sizeof(summary),
             "%s start_offset %.3f, tempo_scale %.3f, matched %d, pitch errors %d, rhythm deviations %d, duration info %d, missing notes %d, extra notes %d, low confidence %d.",
             performance_start_delay > 1.0f ? "已忽略开头静音/噪声；" : "已使用两遍鲁棒对齐；",
             start_offset,
             tempo_scale,
             matched_count,
             pitch_error_count,
             rhythm_error_count,
             duration_error_count,
             missing_count,
             extra_count,
             low_confidence_count);
    snprintf(summary,
             sizeof(summary),
             "note DP scoring: start_offset %.3f, tempo_scale %.3f, aligned %d, good %d, frame recovered %d, missing %d, extra %d, trim applied %d, trim candidate %d%s.",
             start_offset,
             tempo_scale,
             aligned_count,
             good_count,
             frame_recovered_count,
             missing_count,
             extra_count,
             leading_trim_count,
             leading_trim_candidate_count,
             leading_trim_suppressed ? " (candidate not applied)" : "");

    if (details_json.failed) {
        details_truncated = true;
    }

    score_json_builder_t result_json = { 0 };
    score_json_append_raw(&result_json, "{\"ok\":true,\"title\":");
    score_json_append_escaped(&result_json, title != NULL && title[0] != '\0' ? title : "untitled");
    score_json_appendf(&result_json,
                       ",\"target_count\":%d,\"played_count\":%d,"
                       "\"raw_played_count\":%d,\"merged_played_count\":%d,"
                       "\"scored_played_count\":%d,\"pitch_frame_count\":%d,"
                       "\"leading_trim_count\":%d,\"leading_trim_candidate_count\":%d,"
                       "\"leading_trim_score\":%.3f,\"leading_trim_suppressed\":%s,"
                       "\"ignored_played_count\":%d,\"matched_count\":%d,"
                       "\"aligned_count\":%d,\"good_count\":%d,"
                       "\"frame_recovered_count\":%d,"
                       "\"pitch_score\":%.1f,\"rhythm_score\":%.1f,"
                       "\"duration_score\":%.1f,\"stability_score\":%.1f,"
                       "\"complete_score\":%.1f,\"total_score\":%.1f,"
                       "\"alignment_method\":\"edit_distance_offset_tempo_gate\","
                       "\"rhythm_method\":\"relative_interval_rhythm\","
                       "\"tempo_scale\":%.4f,\"start_offset\":%.4f,"
                       "\"rough_gate_scale\":%.2f,"
                       "\"fine_gate_scale\":%.2f,\"performance_start_delay\":%.4f,"
                       "\"extra_count\":%d,\"missing_count\":%d,"
                       "\"low_confidence_count\":%d,\"pitch_error_count\":%d,"
                       "\"rhythm_error_count\":%d,\"duration_error_count\":%d,"
                       "\"extra_penalty\":%.1f,\"missing_penalty\":%.1f,"
                       "\"low_confidence_penalty\":%.1f,\"details_truncated\":%s,"
                       "\"details_count\":%d,\"details_limit\":%d,\"summary\":",
                       target_count,
                       input_played_count,
                       played_count,
                       input_played_count,
                       score_played_count,
                       frame_count,
                       leading_trim_count,
                       leading_trim_candidate_count,
                       leading_trim_score,
                       leading_trim_suppressed ? "true" : "false",
                       ignored_played_count,
                       aligned_count,
                       aligned_count,
                       good_count,
                       frame_recovered_count,
                       pitch_score,
                       rhythm_score,
                       duration_score,
                       stability_score,
                       complete_score,
                       total_score,
                       tempo_scale,
                       start_offset,
                       SCORE_ROUGH_GATE_SCALE,
                       SCORE_FINE_GATE_SCALE,
                       performance_start_delay,
                       extra_count,
                       missing_count,
                       low_confidence_count,
                       pitch_error_count,
                       rhythm_error_count,
                       duration_error_count,
                       extra_penalty,
                       missing_penalty,
                       low_confidence_penalty,
                       details_truncated ? "true" : "false",
                       detail_count,
                       SCORE_MAX_DETAIL_ITEMS);
    score_json_append_escaped(&result_json, summary);
    score_json_append_raw(&result_json, ",\"details\":[");
    if (details_json.data != NULL && details_json.len > 0 && !details_json.failed) {
        score_json_append_raw(&result_json, details_json.data);
    }
    score_json_append_raw(&result_json, "]}");

    char *json = NULL;
    if (!result_json.failed && result_json.data != NULL) {
        json = result_json.data;
        result_json.data = NULL;
    }
    free(details_json.data);
    free(result_json.data);
    free(steps);
    free(used_recovery_frames);
    free(merged_played);
    if (json == NULL) {
        json = build_compact_result_json(title,
                                         target_count,
                                         input_played_count,
                                         played_count,
                                         input_played_count,
                                         score_played_count,
                                         frame_count,
                                         aligned_count,
                                         good_count,
                                         frame_recovered_count,
                                         leading_trim_count,
                                         leading_trim_score,
                                         ignored_played_count,
                                         pitch_score,
                                         rhythm_score,
                                         duration_score,
                                         stability_score,
                                         complete_score,
                                         total_score,
                                         tempo_scale,
                                         start_offset,
                                         performance_start_delay,
                                         extra_count,
                                         missing_count,
                                         low_confidence_count,
                                         pitch_error_count,
                                         rhythm_error_count,
                                         duration_error_count,
                                         extra_penalty,
                                         missing_penalty,
                                         low_confidence_penalty);
    }
    return json;
}

char *score_engine_build_result_json(const char *title,
                                     const target_note_t *target,
                                     int target_count,
                                     const played_note_t *played,
                                     int played_count,
                                     float performance_start_delay)
{
    return score_engine_build_result_json_internal(title,
                                                   target,
                                                   target_count,
                                                   played,
                                                   played_count,
                                                   NULL,
                                                   0,
                                                   performance_start_delay);
}

char *score_engine_build_result_json_with_frames(const char *title,
                                                 const target_note_t *target,
                                                 int target_count,
                                                 const played_note_t *played,
                                                 int played_count,
                                                 const pitch_frame_t *frames,
                                                 int frame_count,
                                                 float performance_start_delay)
{
    return score_engine_build_result_json_internal(title,
                                                   target,
                                                   target_count,
                                                   played,
                                                   played_count,
                                                   frames,
                                                   frame_count,
                                                   performance_start_delay);
}

static bool frame_is_valid(const pitch_frame_t *frame, float min_confidence)
{
    return frame != NULL &&
           frame->midi >= 0 &&
           finite_float(frame->time) &&
           frame->time >= 0.0f &&
           frame->confidence >= min_confidence;
}

static bool frame_recovery_find_match(const target_note_t *target,
                                      const pitch_frame_t *frames,
                                      int frame_count,
                                      const alignment_context_t *ctx,
                                      const bool *used_frames,
                                      frame_recovery_match_t *out)
{
    if (target == NULL || frames == NULL || frame_count <= 0 || ctx == NULL || out == NULL) {
        return false;
    }

    float adjusted_start = adjusted_target_start_value(target, ctx);
    float adjusted_duration = adjusted_target_duration_value(target, ctx);
    float adjusted_end = adjusted_start + adjusted_duration;
    float window_start = adjusted_start - SCORE_FRAME_RECOVERY_PAD_SEC;
    float window_end = adjusted_end + SCORE_FRAME_RECOVERY_PAD_SEC;
    float target_midi = target_midi_value(target);
    float hard_time_gate = hard_time_gate_value(target, ctx);
    float best_cost = FLT_MAX;
    frame_recovery_match_t best = {
        .matched = false,
        .frame_idx = -1,
        .adjusted_target_start = adjusted_start,
        .adjusted_target_end = adjusted_end,
        .hard_time_gate = hard_time_gate,
    };

    for (int i = 0; i < frame_count; ++i) {
        const pitch_frame_t *frame = &frames[i];
        if (used_frames != NULL && used_frames[i]) {
            continue;
        }
        if (!finite_float(frame->time)) {
            continue;
        }
        if (frame->time < window_start) {
            continue;
        }
        if (frame->time > window_end) {
            break;
        }
        if (!frame_is_valid(frame, SCORE_FRAME_RECOVERY_MIN_CONFIDENCE)) {
            continue;
        }

        float frame_midi = (float)frame->midi;
        float pitch_error = fabsf(frame_midi - target_midi);
        if (pitch_error > SCORE_FRAME_RECOVERY_MIDI_TOL) {
            continue;
        }

        float time_error = 0.0f;
        if (frame->time < adjusted_start) {
            time_error = adjusted_start - frame->time;
        } else if (frame->time > adjusted_end) {
            time_error = frame->time - adjusted_end;
        }

        float confidence_penalty = (1.0f - clamp_float(frame->confidence, 0.0f, 1.0f)) * 0.75f;
        float cost = pitch_error * 2.0f +
                     time_error / max_float(0.001f, SCORE_FRAME_RECOVERY_PAD_SEC) +
                     confidence_penalty;

        if (cost < best_cost) {
            best_cost = cost;
            best.matched = true;
            best.frame_idx = i;
            best.frame_time = frame->time;
            best.frame_midi = frame_midi;
            best.pitch_error = pitch_error;
            best.time_error = time_error;
            best.match_cost = cost;
            best.confidence = frame->confidence;
        }
    }

    if (!best.matched) {
        return false;
    }

    *out = best;
    return true;
}

static float frame_pitch_error_with_octave(float played_midi,
                                           float target_midi,
                                           bool *octave_corrected)
{
    return octave_aware_pitch_error(played_midi,
                                    target_midi,
                                    octave_corrected,
                                    NULL,
                                    NULL);
}

static float frame_window_pad_sec(const target_note_t *target, float tempo_scale)
{
    float duration = target_duration_value(target) * tempo_scale;
    return max_float(0.16f, duration * 0.45f);
}

static bool frame_find_best_match(const target_note_t *target,
                                  const pitch_frame_t *frames,
                                  int frame_count,
                                  float tempo_scale,
                                  float start_offset,
                                  int min_frame_idx,
                                  frame_match_t *out)
{
    if (target == NULL || frames == NULL || frame_count <= 0 || out == NULL) {
        return false;
    }

    float target_start = start_offset + target_start_time(target) * tempo_scale;
    float target_duration = max_float(0.08f, target_duration_value(target) * tempo_scale);
    float target_end = target_start + target_duration;
    float pad = frame_window_pad_sec(target, tempo_scale);
    float window_start = target_start - pad;
    float window_end = target_end + pad;
    float target_midi = target_midi_value(target);
    float best_cost = FLT_MAX;
    frame_match_t best = {
        .matched = false,
        .frame_idx = -1,
        .adjusted_target_start = target_start,
        .adjusted_target_end = target_end,
    };

    if (window_end < 0.0f) {
        return false;
    }

    int start_idx = min_frame_idx + 1;
    if (start_idx < 0) {
        start_idx = 0;
    }

    for (int i = start_idx; i < frame_count; ++i) {
        const pitch_frame_t *frame = &frames[i];
        if (!finite_float(frame->time)) {
            continue;
        }
        if (frame->time < window_start) {
            continue;
        }
        if (frame->time > window_end) {
            break;
        }
        if (!frame_is_valid(frame, SCORE_FRAME_MIN_CONFIDENCE)) {
            continue;
        }

        bool octave_corrected = false;
        float played_midi = (float)frame->midi;
        float raw_pitch_error = fabsf(played_midi - target_midi);
        float pitch_error = frame_pitch_error_with_octave(played_midi,
                                                          target_midi,
                                                          &octave_corrected);
        if (pitch_error > SCORE_FRAME_PITCH_GATE) {
            continue;
        }

        float time_error = fabsf(frame->time - target_start);
        float time_tolerance = max_float(0.12f, target_duration * 0.55f);
        float confidence_penalty = (1.0f - clamp_float(frame->confidence, 0.0f, 1.0f)) * 0.80f;
        float cost = pitch_error * 1.80f +
                     (time_error / time_tolerance) * 0.90f +
                     confidence_penalty;

        if (cost < best_cost) {
            best_cost = cost;
            best.matched = true;
            best.frame_idx = i;
            best.frame_time = frame->time;
            best.frame_midi = played_midi;
            best.raw_pitch_error = raw_pitch_error;
            best.pitch_error = pitch_error;
            best.time_error = time_error;
            best.match_cost = cost;
            best.confidence = frame->confidence;
            best.octave_corrected = octave_corrected;
        }
    }

    if (!best.matched) {
        return false;
    }

    *out = best;
    return true;
}

static float frame_alignment_candidate_score(const target_note_t *target,
                                             int target_count,
                                             const pitch_frame_t *frames,
                                             int frame_count,
                                             float tempo_scale,
                                             float start_offset)
{
    int eval_count = target_count;
    if (eval_count > SCORE_FRAME_GRID_TARGET_LIMIT) {
        eval_count = SCORE_FRAME_GRID_TARGET_LIMIT;
    }

    int last_frame_idx = -1;
    int matched = 0;
    float cost_sum = 0.0f;
    float time_sum = 0.0f;

    for (int i = 0; i < eval_count; ++i) {
        frame_match_t match;
        if (frame_find_best_match(&target[i],
                                  frames,
                                  frame_count,
                                  tempo_scale,
                                  start_offset,
                                  last_frame_idx,
                                  &match)) {
            matched++;
            last_frame_idx = match.frame_idx;
            cost_sum += match.match_cost;
            time_sum += match.time_error;
        } else {
            cost_sum += 5.0f;
        }
    }

    if (eval_count <= 0) {
        return -FLT_MAX;
    }

    int missing = eval_count - matched;
    return (float)matched * 12.0f -
           (float)missing * 4.0f -
           cost_sum -
           time_sum * 1.50f;
}

static void frame_find_best_timing(const target_note_t *target,
                                   int target_count,
                                   const pitch_frame_t *frames,
                                   int frame_count,
                                   float *out_tempo_scale,
                                   float *out_start_offset)
{
    float best_score = -FLT_MAX;
    float best_tempo = 1.0f;
    float best_offset = 0.0f;

    for (float tempo = SCORE_FRAME_TEMPO_MIN;
         tempo <= SCORE_FRAME_TEMPO_MAX + 0.001f;
         tempo += SCORE_FRAME_TEMPO_STEP) {
        for (float offset = SCORE_FRAME_OFFSET_MIN_SEC;
             offset <= SCORE_FRAME_OFFSET_MAX_SEC + 0.001f;
             offset += SCORE_FRAME_OFFSET_STEP_SEC) {
            float score = frame_alignment_candidate_score(target,
                                                          target_count,
                                                          frames,
                                                          frame_count,
                                                          tempo,
                                                          offset);
            if (score > best_score) {
                best_score = score;
                best_tempo = tempo;
                best_offset = offset;
            }
        }
    }

    float refine_min = best_offset - SCORE_FRAME_OFFSET_REFINE_RANGE_SEC;
    float refine_max = best_offset + SCORE_FRAME_OFFSET_REFINE_RANGE_SEC;
    for (float offset = refine_min;
         offset <= refine_max + 0.001f;
         offset += SCORE_FRAME_OFFSET_REFINE_STEP_SEC) {
        float score = frame_alignment_candidate_score(target,
                                                      target_count,
                                                      frames,
                                                      frame_count,
                                                      best_tempo,
                                                      offset);
        if (score > best_score) {
            best_score = score;
            best_offset = offset;
        }
    }

    if (out_tempo_scale != NULL) {
        *out_tempo_scale = best_tempo;
    }
    if (out_start_offset != NULL) {
        *out_start_offset = best_offset;
    }
}

static const char *frame_detail_result(float pitch_error,
                                       float time_error,
                                       bool octave_corrected)
{
    bool pitch_bad = pitch_error > SCORE_FRAME_GOOD_PITCH_ERROR || octave_corrected;
    bool rhythm_bad = time_error > SCORE_FRAME_GOOD_TIME_ERROR;

    if (!pitch_bad && !rhythm_bad) {
        return "correct";
    }
    if (pitch_bad && !rhythm_bad) {
        return "pitch_error";
    }
    if (!pitch_bad && rhythm_bad) {
        return "rhythm_error";
    }
    return "pitch_and_rhythm_error";
}

static void frame_append_feedback(score_json_builder_t *builder,
                                  int note_index,
                                  float played_midi,
                                  float target_midi,
                                  float pitch_error,
                                  float time_error,
                                  bool octave_corrected,
                                  bool low_confidence)
{
    char feedback[96] = "";
    if (octave_corrected) {
        snprintf(feedback, sizeof(feedback), "note %d possible octave detection", note_index);
    } else if (pitch_error > SCORE_FRAME_GOOD_PITCH_ERROR) {
        snprintf(feedback,
                 sizeof(feedback),
                 played_midi > target_midi ? "note %d high" : "note %d low",
                 note_index);
    } else if (time_error > SCORE_FRAME_GOOD_TIME_ERROR) {
        snprintf(feedback, sizeof(feedback), "note %d timing offset", note_index);
    } else if (low_confidence) {
        snprintf(feedback, sizeof(feedback), "note %d low confidence", note_index);
    }
    score_json_append_escaped(builder, feedback);
}

char *score_engine_build_frame_result_json(const char *title,
                                           const target_note_t *target,
                                           int target_count,
                                           const pitch_frame_t *frames,
                                           int frame_count,
                                           const played_note_t *played,
                                           int played_count,
                                           float performance_start_delay)
{
    if (target_count < 0) {
        target_count = 0;
    }
    if (frame_count < 0) {
        frame_count = 0;
    }
    if (played_count < 0) {
        played_count = 0;
    }
    if (target_count > 0 && target == NULL) {
        target_count = 0;
    }
    if (frame_count > 0 && frames == NULL) {
        frame_count = 0;
    }

    if (target_count <= 0 || frame_count <= 0) {
        return score_engine_build_result_json_with_frames(title,
                                                          target,
                                                          target_count,
                                                          played,
                                                          played_count,
                                                          frames,
                                                          frame_count,
                                                          performance_start_delay);
    }

    frame_match_t *matches = calloc((size_t)target_count, sizeof(frame_match_t));
    bool *used_frames = calloc((size_t)frame_count, sizeof(bool));
    if (matches == NULL || used_frames == NULL) {
        free(matches);
        free(used_frames);
        return score_engine_build_result_json_with_frames(title,
                                                          target,
                                                          target_count,
                                                          played,
                                                          played_count,
                                                          frames,
                                                          frame_count,
                                                          performance_start_delay);
    }

    float tempo_scale = 1.0f;
    float start_offset = 0.0f;
    frame_find_best_timing(target,
                           target_count,
                           frames,
                           frame_count,
                           &tempo_scale,
                           &start_offset);

    int last_frame_idx = -1;
    int matched_count = 0;
    int good_count = 0;
    int missing_count = 0;
    int low_confidence_count = 0;
    int pitch_error_count = 0;
    int rhythm_error_count = 0;
    float pitch_sum = 0.0f;
    float rhythm_sum = 0.0f;
    float confidence_sum = 0.0f;

    score_json_builder_t details_json = { 0 };
    int detail_count = 0;
    bool details_truncated = false;

    for (int i = 0; i < target_count; ++i) {
        frame_match_t match;
        bool ok = frame_find_best_match(&target[i],
                                        frames,
                                        frame_count,
                                        tempo_scale,
                                        start_offset,
                                        last_frame_idx,
                                        &match);
        float target_midi = target_midi_value(&target[i]);
        float adjusted_start = start_offset + target_start_time(&target[i]) * tempo_scale;

        if (!ok) {
            missing_count++;
            if (detail_count < SCORE_MAX_DETAIL_ITEMS && !details_json.failed) {
                if (detail_count > 0) {
                    score_json_append_raw(&details_json, ",");
                }
                score_json_appendf(&details_json,
                                   "{\"index\":%d,\"ref_index\":%d,\"played_index\":-1,"
                                   "\"is_missing\":true,\"is_extra\":false,"
                                   "\"target_midi\":%.2f,",
                                   i + 1,
                                   i + 1,
                                   target_midi);
                score_json_append_note_name(&details_json, "target_note", target_midi);
                score_json_appendf(&details_json,
                                   ",\"target_start\":%.3f,\"adjusted_target_start\":%.3f,"
                                   "\"played_midi\":-1,\"played_note\":\"-\","
                                   "\"played_start\":-1,\"time_error\":0,\"pitch_error\":0,"
                                   "\"raw_pitch_error\":0,\"duration_error\":0,"
                                   "\"match_cost\":0,\"hard_time_gate\":0,"
                                   "\"low_confidence\":false,"
                                   "\"octave_corrected\":false,\"result\":\"missing\","
                                   "\"feedback_text\":",
                                   target_start_time(&target[i]),
                                   adjusted_start);
                char feedback[64];
                snprintf(feedback, sizeof(feedback), "note %d missing", i + 1);
                score_json_append_escaped(&details_json, feedback);
                score_json_append_raw(&details_json, "}");
                if (!details_json.failed) {
                    detail_count++;
                }
            } else {
                details_truncated = true;
            }
            continue;
        }

        matches[i] = match;
        last_frame_idx = match.frame_idx;
        used_frames[match.frame_idx] = true;
        matched_count++;
        if (match.pitch_error <= SCORE_FRAME_GOOD_PITCH_ERROR &&
            match.time_error <= SCORE_FRAME_GOOD_TIME_ERROR &&
            !match.octave_corrected) {
            good_count++;
        }

        bool low_confidence = match.confidence < SCORE_FRAME_STRONG_CONFIDENCE;
        const char *result = frame_detail_result(match.pitch_error,
                                                 match.time_error,
                                                 match.octave_corrected);
        if (low_confidence) {
            low_confidence_count++;
        }
        if (strcmp(result, "pitch_error") == 0 ||
            strcmp(result, "pitch_and_rhythm_error") == 0) {
            pitch_error_count++;
        }
        if (strcmp(result, "rhythm_error") == 0 ||
            strcmp(result, "pitch_and_rhythm_error") == 0) {
            rhythm_error_count++;
        }

        pitch_sum += score_pitch_error(match.pitch_error);
        rhythm_sum += score_rhythm_error(match.time_error);
        confidence_sum += clamp_float(match.confidence, 0.0f, 1.0f) * 100.0f;

        if (detail_count < SCORE_MAX_DETAIL_ITEMS && !details_json.failed) {
            if (detail_count > 0) {
                score_json_append_raw(&details_json, ",");
            }
            score_json_appendf(&details_json,
                               "{\"index\":%d,\"ref_index\":%d,\"played_index\":%d,"
                               "\"is_missing\":false,\"is_extra\":false,"
                               "\"target_midi\":%.2f,",
                               i + 1,
                               i + 1,
                               match.frame_idx + 1,
                               target_midi);
            score_json_append_note_name(&details_json, "target_note", target_midi);
            score_json_appendf(&details_json,
                               ",\"target_start\":%.3f,\"adjusted_target_start\":%.3f,"
                               "\"played_midi\":%.2f,",
                               target_start_time(&target[i]),
                               adjusted_start,
                               match.frame_midi);
            score_json_append_note_name(&details_json, "played_note", match.frame_midi);
            score_json_appendf(&details_json,
                               ",\"played_start\":%.3f,\"played_duration\":0.000,"
                               "\"time_error\":%.3f,\"pitch_error\":%.3f,"
                               "\"raw_pitch_error\":%.3f,\"duration_error\":0,"
                               "\"match_cost\":%.2f,\"hard_time_gate\":0,"
                               "\"confidence\":%.2f,"
                               "\"low_confidence\":%s,\"octave_corrected\":%s,"
                               "\"result\":\"%s\",\"feedback_text\":",
                               match.frame_time,
                               match.time_error,
                               match.pitch_error,
                               match.raw_pitch_error,
                               match.match_cost,
                               match.confidence,
                               low_confidence ? "true" : "false",
                               match.octave_corrected ? "true" : "false",
                               result);
            frame_append_feedback(&details_json,
                                  i + 1,
                                  match.frame_midi,
                                  target_midi,
                                  match.pitch_error,
                                  match.time_error,
                                  match.octave_corrected,
                                  low_confidence);
            score_json_append_raw(&details_json, "}");
            if (!details_json.failed) {
                detail_count++;
            }
        } else {
            details_truncated = true;
        }
    }

    int extra_count = 0;
    for (int i = 0; i < frame_count;) {
        if (used_frames[i] ||
            !frame_is_valid(&frames[i], SCORE_FRAME_STRONG_CONFIDENCE)) {
            i++;
            continue;
        }

        int start_idx = i;
        int end_idx = i;
        float midi_sum = (float)frames[i].midi;
        float confidence_sum_extra = frames[i].confidence;
        int point_count = 1;
        i++;

        while (i < frame_count &&
               !used_frames[i] &&
               frame_is_valid(&frames[i], SCORE_FRAME_STRONG_CONFIDENCE) &&
               (frames[i].time - frames[end_idx].time) <= SCORE_FRAME_EXTRA_GAP_SEC &&
               fabsf((float)frames[i].midi - (midi_sum / (float)point_count)) <= SCORE_FRAME_EXTRA_MIDI_TOL) {
            end_idx = i;
            midi_sum += (float)frames[i].midi;
            confidence_sum_extra += frames[i].confidence;
            point_count++;
            i++;
        }

        float avg_confidence = confidence_sum_extra / (float)point_count;
        if (point_count < 2 && avg_confidence < 0.55f) {
            continue;
        }

        extra_count++;
        if (detail_count < SCORE_MAX_DETAIL_ITEMS && !details_json.failed) {
            float played_midi = midi_sum / (float)point_count;
            if (detail_count > 0) {
                score_json_append_raw(&details_json, ",");
            }
            score_json_appendf(&details_json,
                               "{\"index\":%d,\"ref_index\":-1,\"played_index\":%d,"
                               "\"is_missing\":false,\"is_extra\":true,"
                               "\"target_midi\":-1,\"target_note\":\"-\","
                               "\"played_midi\":%.2f,",
                               target_count + extra_count,
                               start_idx + 1,
                               played_midi);
            score_json_append_note_name(&details_json, "played_note", played_midi);
            score_json_appendf(&details_json,
                               ",\"target_start\":-1,\"adjusted_target_start\":-1,"
                               "\"played_start\":%.3f,\"played_duration\":%.3f,"
                               "\"time_error\":0,\"pitch_error\":0,\"raw_pitch_error\":0,"
                               "\"duration_error\":0,\"match_cost\":0,\"hard_time_gate\":0,"
                               "\"confidence\":%.2f,"
                               "\"low_confidence\":false,\"octave_corrected\":false,"
                               "\"result\":\"extra\",\"feedback_text\":\"extra note\"}",
                               frames[start_idx].time,
                               frames[end_idx].time - frames[start_idx].time,
                               avg_confidence);
            if (!details_json.failed) {
                detail_count++;
            }
        } else {
            details_truncated = true;
        }
    }

    float pitch_score = 0.0f;
    float rhythm_score = 0.0f;
    float duration_score = 100.0f;
    float stability_score = 100.0f;
    float complete_score = 0.0f;

    if (matched_count > 0) {
        pitch_score = clamp_float(pitch_sum / (float)matched_count, 0.0f, 100.0f);
        rhythm_score = clamp_float(rhythm_sum / (float)matched_count, 0.0f, 100.0f);
        stability_score = clamp_float(confidence_sum / (float)matched_count, 0.0f, 100.0f);
    }
    if (target_count > 0) {
        complete_score = clamp_float((float)matched_count * 100.0f / (float)target_count,
                                     0.0f,
                                     100.0f);
    }

    float extra_penalty = 0.0f;
    float missing_penalty = 0.0f;
    float low_confidence_penalty = 0.0f;
    float total_score = 0.55f * pitch_score +
                        0.35f * rhythm_score +
                        0.10f * complete_score;
    total_score = clamp_float(total_score, 0.0f, 100.0f);

    char summary[360];
    snprintf(summary,
             sizeof(summary),
             "frame alignment used %d pitch frames, tempo_scale %.3f, start_offset %.3f, matched %d/%d, missing %d, extra segments %d.",
             frame_count,
             tempo_scale,
             start_offset,
             matched_count,
             target_count,
             missing_count,
             extra_count);

    if (details_json.failed) {
        details_truncated = true;
    }

    score_json_builder_t result_json = { 0 };
    score_json_append_raw(&result_json, "{\"ok\":true,\"title\":");
    score_json_append_escaped(&result_json, title != NULL && title[0] != '\0' ? title : "untitled");
    score_json_appendf(&result_json,
                       ",\"target_count\":%d,\"played_count\":%d,"
                       "\"raw_played_count\":%d,\"merged_played_count\":%d,"
                       "\"scored_played_count\":%d,\"pitch_frame_count\":%d,"
                       "\"leading_trim_count\":0,\"leading_trim_score\":0,"
                       "\"ignored_played_count\":0,\"matched_count\":%d,"
                       "\"aligned_count\":%d,\"good_count\":%d,"
                       "\"frame_recovered_count\":0,\"pitch_score\":%.1f,"
                       "\"rhythm_score\":%.1f,\"duration_score\":%.1f,"
                       "\"stability_score\":%.1f,\"complete_score\":%.1f,"
                       "\"total_score\":%.1f,"
                       "\"alignment_method\":\"frame_grid_monotonic_alignment\","
                       "\"rhythm_method\":\"frame_start_time_error\","
                       "\"tempo_scale\":%.4f,\"start_offset\":%.4f,"
                       "\"rough_gate_scale\":0,\"fine_gate_scale\":0,"
                       "\"performance_start_delay\":%.4f,"
                       "\"extra_count\":%d,\"missing_count\":%d,"
                       "\"low_confidence_count\":%d,\"pitch_error_count\":%d,"
                       "\"rhythm_error_count\":%d,\"duration_error_count\":0,"
                       "\"extra_penalty\":%.1f,\"missing_penalty\":%.1f,"
                       "\"low_confidence_penalty\":%.1f,\"details_truncated\":%s,"
                       "\"details_count\":%d,\"details_limit\":%d,\"summary\":",
                       target_count,
                       played_count,
                       played_count,
                       played_count,
                       played_count,
                       frame_count,
                       matched_count,
                       matched_count,
                       good_count,
                       pitch_score,
                       rhythm_score,
                       duration_score,
                       stability_score,
                       complete_score,
                       total_score,
                       tempo_scale,
                       start_offset,
                       performance_start_delay,
                       extra_count,
                       missing_count,
                       low_confidence_count,
                       pitch_error_count,
                       rhythm_error_count,
                       extra_penalty,
                       missing_penalty,
                       low_confidence_penalty,
                       details_truncated ? "true" : "false",
                       detail_count,
                       SCORE_MAX_DETAIL_ITEMS);
    score_json_append_escaped(&result_json, summary);
    score_json_append_raw(&result_json, ",\"details\":[");
    if (details_json.data != NULL && details_json.len > 0 && !details_json.failed) {
        score_json_append_raw(&result_json, details_json.data);
    }
    score_json_append_raw(&result_json, "]}");

    char *json = NULL;
    if (!result_json.failed && result_json.data != NULL) {
        json = result_json.data;
        result_json.data = NULL;
    }

    free(details_json.data);
    free(result_json.data);
    free(matches);
    free(used_frames);

    if (json == NULL) {
        return score_engine_build_result_json_with_frames(title,
                                                          target,
                                                          target_count,
                                                          played,
                                                          played_count,
                                                          frames,
                                                          frame_count,
                                                          performance_start_delay);
    }
    return json;
}
