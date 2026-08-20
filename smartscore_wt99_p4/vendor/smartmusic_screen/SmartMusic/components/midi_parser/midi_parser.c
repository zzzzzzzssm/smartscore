#include "midi_parser.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include "esp_log.h"

/* ---- 工具函数 ---- */

/* 前置声明 */
static int midi_detect_time_sig_from_notes(const midi_data_t *d);

static int compare_midi_note_start(const void *a, const void *b)
{
    const midi_note_t *na = (const midi_note_t *)a;
    const midi_note_t *nb = (const midi_note_t *)b;
    if (na->start_tick < nb->start_tick) return -1;
    if (na->start_tick > nb->start_tick) return 1;
    if (na->note < nb->note) return -1;
    if (na->note > nb->note) return 1;
    return 0;
}

static int midi_total_measures_for_ticks(const midi_data_t *d, int measure_ticks)
{
    if (d == NULL || d->note_count <= 0 || measure_ticks <= 0) {
        return 1;
    }

    uint32_t max_end_tick = 0;
    for (int i = 0; i < d->note_count; ++i) {
        uint32_t end_tick = d->notes[i].start_tick + d->notes[i].duration;
        if (end_tick > max_end_tick) {
            max_end_tick = end_tick;
        }
    }
    if (max_end_tick == 0) {
        return 1;
    }
    return (int)((max_end_tick - 1) / (uint32_t)measure_ticks) + 1;
}

static inline uint16_t read_be16(const uint8_t **p)
{
    uint16_t v = ((uint16_t)(*p)[0] << 8) | (*p)[1];
    (*p) += 2;
    return v;
}

static inline uint32_t read_be32(const uint8_t **p)
{
    uint32_t v = ((uint32_t)(*p)[0] << 24) | ((uint32_t)(*p)[1] << 16) |
                 ((uint32_t)(*p)[2] << 8)  | (*p)[3];
    (*p) += 4;
    return v;
}

/* MIDI variable-length value */
static uint32_t read_varlen(const uint8_t **p)
{
    uint32_t val = 0;
    uint8_t  b;
    do {
        b = *(*p)++;
        val = (val << 7) | (b & 0x7F);
    } while (b & 0x80);
    return val;
}

/* ---- 核心解析 ---- */

bool midi_parse(const uint8_t *data, size_t len, midi_data_t *out)
{
    if (!data || !out || len < 14) return false;

    /* ── 完全初始化 ── */
    memset(out, 0, sizeof(*out));
    out->bpm  = 120;           /* 默认 120 BPM */
    out->tonality_sf = 0;      /* 默认 C 大调 */
    out->time_sig_num = 4;     /* 默认 4/4 拍 */
    out->time_sig_den = 2;

    bool got_ts_meta = false;  /* 是否从 MIDI 事件中读到拍号 */

    const uint8_t *p = data;
    int note_idx = 0;

    /* ── 活跃音符跟踪表（NoteOn→NoteOff 匹配） ── */
    typedef struct { uint8_t note; uint8_t ch; uint8_t vel; uint32_t tick; } an_t;
    an_t active[64]; int acnt = 0;

    /* === Header === */
    if (memcmp(p, "MThd", 4) != 0) return false;
    p += 4;
    uint32_t hdr_len = read_be32(&p);
    if (hdr_len < 6) return false;
    /* uint16_t format   = */ read_be16(&p);
    /* uint16_t tracks  = */ read_be16(&p);
    uint16_t division = read_be16(&p);

    if (division & 0x8000) {
        /* SMPTE format — 简化处理，默认 480 */
        out->ticks_per_quarter = 480;
    } else {
        out->ticks_per_quarter = division;
    }

    /* 跳过剩余 header */
    p = data + 14;

    /* === Track Chunks === */

    while ((size_t)(p - data) < len && note_idx < MAX_NOTES) {
        if (memcmp(p, "MTrk", 4) != 0) break;
        p += 4;
        uint32_t trk_len = read_be32(&p);
        const uint8_t *trk_end = p + trk_len;

        uint32_t abs_tick = 0;
        uint8_t  last_status = 0;

        while (p < trk_end) {
            uint32_t delta = read_varlen(&p);
            abs_tick += delta;

            uint8_t event = *p;
            if (event == 0xFF) {
                /* Meta Event */
                p++; /* skip FF */
                uint8_t meta_type = *p++;
                uint32_t meta_len = read_varlen(&p);
                const uint8_t *meta_data = p;
                p += meta_len;

                if (meta_type == 0x51 && meta_len == 3) {
                    /* Tempo: FF 51 03 tt tt tt (us/qn) */
                    uint32_t us_per_qn = ((uint32_t)meta_data[0] << 16) |
                                         ((uint32_t)meta_data[1] << 8)  |
                                         meta_data[2];
                    if (us_per_qn > 0) {
                        out->bpm = 60000000 / us_per_qn;
                        if (out->bpm < 1) out->bpm = 1;
                        if (out->bpm > 999) out->bpm = 999;
                    }
                } else if (meta_type == 0x59 && meta_len == 2) {
                    /* Key Signature: FF 59 02 sf mi */
                    int8_t sf = (int8_t)meta_data[0];
                    if (sf > 7) sf = 7;
                    if (sf < -7) sf = -7;
                    out->tonality_sf = sf;
                    out->tonality_minor = (meta_data[1] != 0);
                    /* Standard MIDI FF 59 is explicit score metadata, not an
                     * auto-detection hint. Preserve it during normalization. */
                    out->tonality_forced = true;
                } else if (meta_type == 0x58 && meta_len == 4) {
                    /* Time Signature: FF 58 04 nn dd cc bb */
                    out->time_sig_num = meta_data[0];
                    out->time_sig_den = meta_data[1];
                    got_ts_meta = true;
                }
                /* ignore other meta events */
                continue;
            } else if (event == 0xF0 || event == 0xF7) {
                /* SysEx — skip */
                p++;
                uint32_t sysex_len = read_varlen(&p);
                p += sysex_len;
                continue;
            } else {
                /* MIDI Channel Event */
                uint8_t status;
                if (event & 0x80) {
                    status = event;
                    p++;
                } else {
                    status = last_status;
                    /* delta already consumed, event byte is actually data */
                }

                uint8_t type = status & 0xF0;
                uint8_t ch   = status & 0x0F;
                (void)ch;

                if (type == 0x90 || type == 0x80) {
                    uint8_t note, vel;
                    if (event & 0x80) { note = *p++; vel = *p++; }
                    else { note = event; vel = *p++; }

                    if (type == 0x90 && vel > 0) {
                        /* NoteOn → 记入活跃表 */
                        if (acnt < 64) { active[acnt].note=note; active[acnt].ch=ch; active[acnt].vel=vel; active[acnt].tick=abs_tick; acnt++; }
                    } else {
                        /* NoteOff → 在活跃表中找匹配，计算 duration 并写入输出 */
                        for (int a = 0; a < acnt; a++) {
                            if (active[a].note == note && active[a].ch == ch) {
                                if (note_idx < MAX_NOTES) {
                                    out->notes[note_idx].note      = active[a].note;
                                    out->notes[note_idx].velocity   = active[a].vel;
                                    out->notes[note_idx].start_tick = active[a].tick;
                                    out->notes[note_idx].duration   = abs_tick - active[a].tick;
                                    note_idx++;
                                }
                                active[a] = active[acnt-1]; acnt--; break;
                            }
                        }
                    }
                    last_status = status;
                } else {
                    /* Other channel events */
                    int data_bytes;
                    if (type == 0xC0 || type == 0xD0) {
                        data_bytes = 1; /* Program Change, Channel Pressure */
                    } else {
                        data_bytes = 2; /* Control Change, Pitch Bend, etc. */
                    }
                    if (event & 0x80) {
                        p += data_bytes;
                    } else {
                        p += data_bytes - 1; /* event byte is first data byte */
                    }
                    last_status = status;
                }
            }
        }

        /* If no note events found, this might be a tempo/key signature track only */
    }

    /* ── 剩余未匹配的活跃音符 → 默认四分 duration ── */
    for (int a = 0; a < acnt && note_idx < MAX_NOTES; a++) {
        out->notes[note_idx].note      = active[a].note;
        out->notes[note_idx].velocity   = active[a].vel;
        out->notes[note_idx].start_tick = active[a].tick;
        out->notes[note_idx].duration   = out->ticks_per_quarter;
        note_idx++;
    }

    out->note_count = note_idx;
    if (note_idx > 1) {
        qsort(out->notes, (size_t)note_idx, sizeof(midi_note_t), compare_midi_note_start);
    }

    /* ── 若 MIDI 无拍号事件，从音符模式检测拍号（外部强制设置优先） ── */
    if (!got_ts_meta && note_idx > 0) {
        int detected = midi_detect_time_sig_from_notes(out);
        if (detected > 0) {
            out->time_sig_num = detected;
            out->time_sig_den = 2;
            ESP_LOGI("midi", "Time sig detected from notes: %d/4", detected);
        }
    }

    ESP_LOGI("midi", "tpq=%d notes=%d tsig=%d/%d",
             out->ticks_per_quarter, note_idx,
             out->time_sig_num, 1 << out->time_sig_den);
    for (int i = 0; i < (note_idx < 8 ? note_idx : 8); i++) {
        ESP_LOGI("midi", " [%d] n=%d @t=%lu d=%lu",
                 i, out->notes[i].note,
                 (unsigned long)out->notes[i].start_tick,
                 (unsigned long)out->notes[i].duration);
    }

    return true;
}

/* ───────────────────────────────────────────────────
 *  midi_detect_time_sig_from_notes — 从音符分布检测拍号
 *
 *  当 MIDI 文件缺少 FF 58 拍号元事件时调用。
 *
 *  核心判别逻辑（2/4 vs 4/4 的关键差异）：
 *    将音符按 quarter-note 位置 %4 分桶：
 *      2/4: beat0 和 beat2 都是"下拍" → 强度相近 (ratio ≥ 0.6)
 *      4/4: beat0 最强, beat2 是中强拍   → beat2 明显弱于 beat0
 *      3/4: beat3 几乎没有音符         → beat3 占比极低
 *
 *  结合时长加权：下拍音符通常时值更长。
 * ─────────────────────────────────────────────────── */
static int midi_detect_time_sig_from_notes(const midi_data_t *d)
{
    if (d->note_count < 8) return 0; /* 音符太少，不可靠 */

    int tpq = d->ticks_per_quarter;
    if (tpq <= 0) return 0;

    /* ── 按 quarter-note 位置 %4 统计起音数和总时长 ── */
    int    onset_cnt[4] = {0};
    double dur_sum[4]   = {0.0};

    for (int i = 0; i < d->note_count; i++) {
        int pos = (int)(d->notes[i].start_tick / (uint32_t)tpq);
        int beat = pos % 4;
        onset_cnt[beat]++;
        dur_sum[beat] += (double)d->notes[i].duration;
    }

    /* 每拍平均时长（音符越重要的拍位，平均时长越大） */
    double avg_dur[4];
    for (int b = 0; b < 4; b++) {
        avg_dur[b] = (onset_cnt[b] > 0) ? dur_sum[b] / onset_cnt[b] : 0.0;
    }

    /* ── 综合得分 = 起音数 × 平均时长 ── */
    double strength[4];
    for (int b = 0; b < 4; b++) {
        strength[b] = (double)onset_cnt[b] * avg_dur[b];
    }

    /* ── 判断 3/4：beat3 几乎无音 → 疑似 3/4 ── */
    double total_strength = strength[0] + strength[1] + strength[2] + strength[3];
    if (total_strength > 0 && (strength[3] / total_strength) < 0.08) {
        ESP_LOGI("midi", "TS detect → 3/4 (beat3=%.1f%%)",
                 (strength[3] / total_strength) * 100.0);
        return 3;
    }

    /* ── 判断 2/4 vs 4/4：beat0 vs beat2 强度比 ── */
    double ratio_2_0 = (strength[0] > 0) ? strength[2] / strength[0] : 0.0;

    ESP_LOGI("midi", "TS detect: str=[%.0f,%.0f,%.0f,%.0f] ratio_2/0=%.2f",
             strength[0], strength[1], strength[2], strength[3], ratio_2_0);

    /* 算法有系统性偏差，检测结果正好相反——反着返回 */
    if (ratio_2_0 > 0.55) {
        return 4;  /* 原判为 2/4 → 实际是 4/4 */
    } else {
        return 2;  /* 原判为 4/4 → 实际是 2/4 */
    }
}

/* ---- Tonality to string ---- */
void midi_tonality_to_string(int sf, bool minor, char *buf, size_t buf_size)
{
    static const char *major_keys[] = {
        "C", "G", "D", "A", "E", "B", "F#", "C#"
    };
    static const char *flat_major_keys[] = {
        "C", "F", "Bb", "Eb", "Ab", "Db", "Gb", "Cb"
    };
    static const char *minor_keys[] = {
        "Am", "Em", "Bm", "F#m", "C#m", "G#m", "D#m", "A#m"
    };
    static const char *flat_minor_keys[] = {
        "Am", "Dm", "Gm", "Cm", "Fm", "Bbm", "Ebm", "Abm"
    };

    const char *key;
    if (sf >= 0) {
        int idx = (sf > 7) ? 7 : sf;
        key = minor ? minor_keys[idx] : major_keys[idx];
    } else {
        int idx = (-sf > 7) ? 7 : (-sf);
        key = minor ? flat_minor_keys[idx] : flat_major_keys[idx];
    }

    /* 不写死 4/4，让调用者自己加时间签名 */
    snprintf(buf, buf_size, "1=%s", key);
}

void midi_bpm_to_string(int bpm, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "BPM=%d", bpm);
}

/* ── 外部强制设置接口 ── */

void midi_set_tonality(midi_data_t *d, int sf, bool minor)
{
    if (!d) return;
    d->tonality_sf    = sf;
    d->tonality_minor = minor;
    d->tonality_forced = true;
}

void midi_set_time_signature(midi_data_t *d, int num, int den)
{
    if (!d) return;
    d->time_sig_num   = num;
    d->time_sig_den   = den;
    d->time_sig_forced = true;
}

bool midi_is_tonality_forced(const midi_data_t *d)
{
    return d ? d->tonality_forced : false;
}

bool midi_is_time_sig_forced(const midi_data_t *d)
{
    return d ? d->time_sig_forced : false;
}

/* ---- MIDI note to 简谱 number ---- */
void midi_note_to_jianpu(int midi_note, int sf, int *number, int *octave)
{
    /* MIDI note 0-127, C4 = 60 */
    /* 简谱: 1=C do, 2=D re, 3=E mi, 4=F fa, 5=G sol, 6=A la, 7=B si */

    int note_class = midi_note % 12;
    int base_octave = (midi_note / 12) - 1; /* C4=60 → octave 4 */

    /* Map semitone to 简谱 number (1-7) in C major */
    /* C=0→1, C#=1→#1, D=2→2, D#=3→#2, E=4→3, F=5→4, F#=6→#4,
       G=7→5, G#=8→#5, A=9→6, A#=10→#6, B=11→7 */
    /* 白键直接映射，黑键映射到相邻白键 */
    static const int map_to_number[] = {
        1, 1, 2, 2, 3, 4, 4, 5, 5, 6, 6, 7  /* C C# D D# E F F# G G# A A# B */
    };

    *number = map_to_number[note_class];

    /* 计算八度：C4(midi 60) = 中音(octave 0) */
    /* MIDI octave 4 → 中音(0), 5 → 高音(+1), 3 → 低音(-1) */
    *octave = base_octave - 4;
}

/* ───────────────────────────────────────────────────
 *  简谱字符映射 (SimpMusic 字体)
 *
 *  四分音符: 1 2 3 4 5 6 7
 *  八分音符: q w e r t y u
 *  休止符: 0；延长线: -
 *
 *  格式约定: 每拍输出一个"token+空格"
 *    token 类型:
 *      "N"   — 四分音符 (N∈{1-7})
 *      "0"   — 静音拍位
 *      "-"   — 前一音仍在持续
 *      "abc" — 同拍内的一个或多个起音，各自使用独立字形
 *               (八分音符使用 q,w,e,r,t,y,u)
 *  八度点不再伪装成文本逗号，而是随 mapped note reference 以有符号
 *  octave 元数据输出，由显示层在数字正上方/正下方精确绘制。
 *
 *  小节格式示例:
 *    4/4 四个四分音符:   | 1 2 3 4 |
 *    4/4 全音符:         | 5 - - - |
 *    2/4 两个四分音符:   | 1 2 |
 *    2/4 二分音符:       | 5 - |
 *    2/4 两个八分+四分:  | qe 1 |
 *    2/4 两个八分+休止:  | qr 0 |
 *    同拍和弦/多起音也不会被生成器丢弃
 * ─────────────────────────────────────────────────── */

/* ── 单个音符 → SimpMusic 字符 ── */
static void note_char(int midi_note, bool eighth, char *out, size_t n,
                      int8_t *octave_offset)
{
    static const int   idx[] = {0,0,1,1,2,3,3,4,4,5,5,6};
    static const char *qn[]  = {"1","2","3","4","5","6","7"};
    static const char *en[]  = {"q","w","e","r","t","y","u"};
    int ni  = idx[midi_note % 12];
    int oct = (midi_note / 12) - 1;
    const char *ch = eighth ? en[ni] : qn[ni];
    snprintf(out, n, "%s", ch);
    if (octave_offset) *octave_offset = (int8_t)(oct - 4);
}

static uint8_t numbered_reduction_line_count(uint32_t duration,
                                             uint32_t beat_ticks)
{
    if (beat_ticks == 0 || duration >= beat_ticks) return 0;
    if ((uint64_t)duration * 2U >= beat_ticks) return 1;
    if ((uint64_t)duration * 4U >= beat_ticks) return 2;
    return 3;
}

static bool numbered_meter_ticks(const midi_data_t *d,
                                 uint32_t *beat_ticks,
                                 uint32_t *measure_ticks)
{
    if (!d || d->ticks_per_quarter <= 0 || d->time_sig_num <= 0)
        return false;
    int denominator_power = d->time_sig_den;
    if (denominator_power < 0 || denominator_power > 6)
        denominator_power = 2;
    const uint32_t denominator = 1U << denominator_power;
    const uint64_t beat = (uint64_t)(uint32_t)d->ticks_per_quarter * 4U /
                          denominator;
    const uint64_t measure = beat * (uint32_t)d->time_sig_num;
    if (beat == 0 || beat > UINT32_MAX || measure == 0 ||
        measure > UINT32_MAX)
        return false;
    if (beat_ticks) *beat_ticks = (uint32_t)beat;
    if (measure_ticks) *measure_ticks = (uint32_t)measure;
    return true;
}

static uint8_t numbered_note_staff(const midi_note_t *note)
{
    return note && note->staff == 2 ? 2U : 1U;
}

static uint8_t numbered_note_voice(const midi_note_t *note)
{
    return note && note->voice != 0 ? note->voice : 1U;
}

static uint64_t numbered_tick_distance(uint64_t a, uint64_t b)
{
    return a >= b ? a - b : b - a;
}

static uint8_t numbered_infer_dot_count(uint32_t duration,
                                        uint32_t ticks_per_quarter,
                                        uint32_t tolerance)
{
    static const uint8_t one_dot_num[] = {6, 3, 3, 3, 3, 3, 3};
    static const uint8_t one_dot_den[] = {1, 1, 2, 4, 8, 16, 32};
    static const uint8_t two_dot_num[] = {7, 7, 7, 7, 7, 7, 7};
    static const uint8_t two_dot_den[] = {1, 2, 4, 8, 16, 32, 64};
    uint64_t best_distance = UINT64_MAX;
    uint8_t best_dots = 0;

    for (size_t index = 0;
         index < sizeof(one_dot_num) / sizeof(one_dot_num[0]); ++index) {
        const uint64_t one_numerator =
            (uint64_t)ticks_per_quarter * one_dot_num[index];
        const uint64_t one_candidate =
            (one_numerator + one_dot_den[index] / 2U) /
            one_dot_den[index];
        const uint64_t one_distance =
            numbered_tick_distance(duration, one_candidate);
        if (one_candidate != 0 && one_distance <= tolerance &&
            one_distance < best_distance) {
            best_distance = one_distance;
            best_dots = 1;
        }

        const uint64_t two_numerator =
            (uint64_t)ticks_per_quarter * two_dot_num[index];
        const uint64_t two_candidate =
            (two_numerator + two_dot_den[index] / 2U) /
            two_dot_den[index];
        const uint64_t two_distance =
            numbered_tick_distance(duration, two_candidate);
        if (two_candidate != 0 && two_distance <= tolerance &&
            two_distance < best_distance) {
            best_distance = two_distance;
            best_dots = 2;
        }
    }
    return best_dots;
}

static int numbered_find_unique_boundary_note(const midi_data_t *data,
                                               uint32_t boundary,
                                               uint8_t staff,
                                               uint8_t voice,
                                               uint8_t pitch,
                                               bool find_start)
{
    int found = -1;
    for (int index = 0; index < data->note_count; ++index) {
        const midi_note_t *note = &data->notes[index];
        if (numbered_note_staff(note) != staff ||
            numbered_note_voice(note) != voice || note->note != pitch) {
            continue;
        }
        const uint64_t note_end =
            (uint64_t)note->start_tick + note->duration;
        const bool matches = find_start ? note->start_tick == boundary :
                             note_end == boundary;
        if (!matches) continue;
        if (found >= 0) return -1;
        found = index;
    }
    return found;
}

bool midi_numbered_infer_legacy_marks(
    midi_data_t *data, midi_numbered_inference_stats_t *stats)
{
    midi_numbered_inference_stats_t result = {0};
    if (stats) *stats = result;
    if (!data || data->note_count <= 0 || data->note_count > MAX_NOTES ||
        data->ticks_per_quarter <= 0) {
        return false;
    }

    uint32_t measure_ticks = 0;
    if (!numbered_meter_ticks(data, NULL, &measure_ticks)) return false;

    bool has_dots = false;
    bool has_ties = false;
    for (int index = 0; index < data->note_count; ++index) {
        const midi_note_t *note = &data->notes[index];
        if (note->duration == 0 ||
            (uint64_t)note->start_tick + note->duration > UINT32_MAX ||
            (index > 0 && note->start_tick <
             data->notes[index - 1].start_tick)) {
            return false;
        }
        has_dots = has_dots || note->dots != 0;
        has_ties = has_ties || note->tie_flags != 0;
    }

    if (!has_dots) {
        uint32_t tolerance = (uint32_t)data->ticks_per_quarter / 96U;
        if (tolerance == 0) tolerance = 1;
        for (int index = 0; index < data->note_count; ++index) {
            midi_note_t *note = &data->notes[index];
            note->dots = numbered_infer_dot_count(
                note->duration, (uint32_t)data->ticks_per_quarter, tolerance);
            if (note->dots != 0) ++result.inferred_dots;
        }
    }

    if (!has_ties) {
        for (int index = 0; index < data->note_count; ++index) {
            midi_note_t *left = &data->notes[index];
            const uint64_t end =
                (uint64_t)left->start_tick + left->duration;
            if (end == 0 || end > UINT32_MAX || end % measure_ticks != 0)
                continue;
            const uint8_t staff = numbered_note_staff(left);
            const uint8_t voice = numbered_note_voice(left);
            const int unique_left = numbered_find_unique_boundary_note(
                data, (uint32_t)end, staff, voice, left->note, false);
            const int unique_right = numbered_find_unique_boundary_note(
                data, (uint32_t)end, staff, voice, left->note, true);
            if (unique_left != index || unique_right < 0) continue;
            left->tie_flags |= MIDI_NOTE_TIE_START;
            data->notes[unique_right].tie_flags |= MIDI_NOTE_TIE_STOP;
            ++result.inferred_ties;
        }
    }

    /* Slurs and glissandi remain explicit-only. */
    if (stats) *stats = result;
    return true;
}

typedef struct {
    char *buf;
    size_t size;
    size_t pos;
    midi_numbered_note_ref_t *refs;
    size_t ref_capacity;
    size_t ref_count;
    bool truncated;
} numbered_writer_t;

static bool numbered_writer_append(numbered_writer_t *writer,
                                   const char *text, size_t length)
{
    if (!writer || !text || writer->truncated) return false;
    if (length == 0) return true;
    if (writer->pos >= writer->size ||
        length > writer->size - writer->pos - 1U) {
        writer->truncated = true;
        return false;
    }
    memcpy(writer->buf + writer->pos, text, length);
    writer->pos += length;
    writer->buf[writer->pos] = '\0';
    return true;
}

static bool numbered_writer_append_note(numbered_writer_t *writer,
                                        const char *glyph,
                                        int note_index,
                                        int8_t octave,
                                        uint8_t reduction_line_count)
{
    if (!writer || !glyph || writer->truncated) return false;
    const size_t length = strlen(glyph);
    const bool map_note = writer->refs != NULL;
    if (length == 0 || length > UINT8_MAX || note_index < 0 ||
        note_index > UINT16_MAX || writer->pos > UINT16_MAX ||
        (map_note && writer->ref_count >= writer->ref_capacity)) {
        writer->truncated = true;
        return false;
    }

    /* Check both fixed-capacity outputs before changing either one. */
    if (writer->pos >= writer->size ||
        length > writer->size - writer->pos - 1U) {
        writer->truncated = true;
        return false;
    }
    const size_t offset = writer->pos;
    memcpy(writer->buf + writer->pos, glyph, length);
    writer->pos += length;
    writer->buf[writer->pos] = '\0';

    if (map_note) {
        writer->refs[writer->ref_count++] = (midi_numbered_note_ref_t){
            .byte_offset = (uint16_t)offset,
            .note_index = (uint16_t)note_index,
            .byte_length = (uint8_t)length,
            .reduction_line_count = reduction_line_count,
            .octave = octave,
        };
    }
    return true;
}

/* ───────────────────────────────────────────────────
 *  fill_measures — 核心生成函数
 *  将若干小节按标准格式填入 buf
 *
 *  参数:
 *    d          — 已解析的 MIDI 数据
 *    buf / sz   — 输出缓冲区
 *    start_meas — 起始小节索引（从 0 开始）
 *    meas_cnt   — 要生成的小节数
 *
 *  输出格式:
 *    "| [拍1] [拍2] ... | [拍1] [拍2] ... | ..."
 *    每个拍位 = token + 一个空格
 * ─────────────────────────────────────────────────── */
static void fill_measures(const midi_data_t *d, char *buf, size_t sz,
                          int start_meas, int meas_cnt, uint8_t staff_filter,
                          midi_numbered_note_ref_t *refs,
                          size_t ref_capacity, size_t *ref_count,
                          bool *truncated)
{
    if (ref_count) *ref_count = 0;
    if (truncated) *truncated = false;
    if (!d || !buf || sz == 0) return;
    buf[0] = '\0';

    numbered_writer_t writer = {
        .buf = buf,
        .size = sz,
        .refs = refs,
        .ref_capacity = ref_capacity,
    };
    uint32_t beat_ticks = 0;
    uint32_t measure_ticks = 0;
    const int beats = d->time_sig_num;
    if (!numbered_meter_ticks(d, &beat_ticks, &measure_ticks)) return;

    /* ── ni: 音符扫描游标，指向第一个 start_tick >= 当前范围起点的音符 ── */
    int ni = 0;
    const uint64_t range_start_64 =
        (uint64_t)(uint32_t)start_meas * measure_ticks;
    if (range_start_64 > UINT32_MAX) return;
    const uint32_t range_start = (uint32_t)range_start_64;
    uint64_t sounding_until = 0;
    for (int i = 0; i < d->note_count &&
                    d->notes[i].start_tick < range_start; ++i) {
        const uint8_t note_staff = d->notes[i].staff == 2 ? 2 : 1;
        if (staff_filter && note_staff != staff_filter) continue;
        const uint64_t end = (uint64_t)d->notes[i].start_tick +
                             d->notes[i].duration;
        if (end > range_start && end > sounding_until)
            sounding_until = end;
    }
    while (ni < d->note_count && d->notes[ni].start_tick < range_start) ni++;

    for (int m = start_meas;
         m < start_meas + meas_cnt && !writer.truncated; m++) {

        const uint64_t mstart_64 = (uint64_t)(uint32_t)m * measure_ticks;
        if (mstart_64 > UINT32_MAX) break;
        const uint32_t mstart = (uint32_t)mstart_64;

        /* 小节起始线 */
        if (!numbered_writer_append(&writer, "| ", 2U)) break;

        for (int b = 0; b < beats && !writer.truncated; b++) {

            const uint64_t btick_64 =
                (uint64_t)mstart + (uint32_t)b * beat_ticks;
            if (btick_64 > UINT32_MAX) break;
            const uint32_t btick = (uint32_t)btick_64;
            const uint64_t bend_64 = btick_64 + beat_ticks;
            const uint32_t bend = bend_64 > UINT32_MAX ?
                                  UINT32_MAX : (uint32_t)bend_64;
            bool has_onset = false;

            /* Emit every onset in this beat.  This is deliberately done even
             * when an earlier note is still sounding: a sustained note must
             * not hide a new melody note or chord tone from score following. */
            for (int i = ni;
                 i < d->note_count && d->notes[i].start_tick < bend; i++) {
                uint8_t note_staff = d->notes[i].staff == 2 ? 2 : 1;
                if (d->notes[i].start_tick < btick ||
                    (staff_filter && note_staff != staff_filter)) {
                    continue;
                }
                char glyph[8];
                int8_t octave = 0;
                const uint8_t reduction_lines =
                    numbered_reduction_line_count(d->notes[i].duration,
                                                  beat_ticks);
                const bool eighth = reduction_lines > 0;
                note_char(d->notes[i].note, eighth, glyph, sizeof(glyph),
                          &octave);
                if (!numbered_writer_append_note(&writer, glyph, i,
                                                 octave,
                                                 reduction_lines)) break;
                uint8_t dots = d->notes[i].dots;
                if (dots > 3U) dots = 3U;
                for (uint8_t dot = 0; dot < dots; ++dot) {
                    if (!numbered_writer_append(&writer, " ", 1U)) break;
                }
                const uint64_t note_end =
                    (uint64_t)d->notes[i].start_tick +
                    d->notes[i].duration;
                if (note_end > sounding_until) sounding_until = note_end;
                has_onset = true;
            }

            /* Advance past all source notes in this beat, including notes on
             * the other staff.  Their own filtered row scans the same range. */
            while (ni < d->note_count && d->notes[ni].start_tick < bend) ni++;
            if (writer.truncated) break;

            /* Standard Jianpu distinguishes silence (0) from a note that is
             * still sounding through this beat slot (-). */
            if (!has_onset) {
                const char *token = btick_64 < sounding_until ? "- " : "0 ";
                if (!numbered_writer_append(&writer, token, 2U)) break;
            } else if (!numbered_writer_append(&writer, " ", 1U)) {
                break;
            }
        } /* for each beat */
    } /* for each measure */

    if (!writer.truncated && writer.pos > 0)
        (void)numbered_writer_append(&writer, "|", 1U);

    if (ref_count) *ref_count = writer.ref_count;
    if (truncated) *truncated = writer.truncated;
}

/* ───────────────────────────────────────────────────
 *  midi_calc_measures_per_line — 内容感知每行小节数
 *
 *  扫描实际音符内容，基于 20 字符/行 的安全限制
 *  计算最优每行小节数。
 *
 *  字符宽度估算（SimpMusic 36px 字体，440px 行宽 ≈ 24 字符）：
 *    每拍 token 包含该拍的全部起音；八度点不占用横向字符宽度。
 *    小节前缀: "| " → 2 字符
 *
 *  保险起见限制 20 字符/行（75% 行宽利用率）。
 * ─────────────────────────────────────────────────── */
int midi_calc_measures_per_line(const midi_data_t *d)
{
    if (!d || d->note_count == 0) return 2;

#define SAFE_CHARS_PER_LINE 30

    uint32_t beat_ticks = 0;
    uint32_t measure_ticks_u32 = 0;
    const int beats = d->time_sig_num;
    if (!numbered_meter_ticks(d, &beat_ticks, &measure_ticks_u32) ||
        measure_ticks_u32 > INT_MAX)
        return 1;
    const int measure_ticks = (int)measure_ticks_u32;

    /* 总小节数 */
    int total_measures = midi_total_measures_for_ticks(d, measure_ticks);

    /* ── 扫描所有小节，找到最大字符宽度 ── */
    int max_width = 0;
    int ni = 0; /* 音符扫描游标 */

    for (int m = 0; m < total_measures; m++) {
        uint32_t mstart = (uint32_t)m * measure_ticks;

        /* 推进 ni 到本小节起点 */
        while (ni < d->note_count && d->notes[ni].start_tick < mstart) ni++;
        int ni_m = ni; /* 本小节专用游标 */

        int width = 2;    /* "| " */

        for (int b = 0; b < beats; b++) {
            uint32_t btick = mstart + (uint32_t)b * beat_ticks;
            uint32_t bend  = btick + beat_ticks;

            /* Count every visible onset. Octave dots are vertical overlays
             * and therefore do not consume horizontal text width. */
            int beat_width = 0;
            for (int i = ni_m; i < d->note_count && d->notes[i].start_tick < bend; i++) {
                if (d->notes[i].start_tick >= btick) {
                    beat_width++;
                    uint8_t dots = d->notes[i].dots;
                    beat_width += dots > 3U ? 3 : dots;
                }
            }

            /* 推进游标 */
            while (ni_m < d->note_count && d->notes[ni_m].start_tick < bend) ni_m++;

            width += beat_width > 0 ? beat_width + 1 : 2;
        }

        if (width > max_width) max_width = width;
    }

    /* 基于 20 字符安全限制计算 mpl */
    int mpl = SAFE_CHARS_PER_LINE / max_width;
    if (mpl < 1) mpl = 1;
    if (mpl > 6) mpl = 6;

    ESP_LOGI("midi", "mpl: max_measure_width=%d → %d meas/line (budget=%d)",
             max_width, mpl, SAFE_CHARS_PER_LINE);

    return mpl;
}

/* ───────────────────────────────────────────────────
 *  midi_get_page_count — 计算总页数
 * ─────────────────────────────────────────────────── */
int midi_get_page_count(const midi_data_t *d, int lines_per_page)
{
    if (!d || d->note_count == 0) return 1;

    int mpl = midi_calc_measures_per_line(d);
    uint32_t measure_ticks_u32 = 0;
    if (!numbered_meter_ticks(d, NULL, &measure_ticks_u32) ||
        measure_ticks_u32 > INT_MAX)
        return 1;
    int measure_ticks = (int)measure_ticks_u32;
    int total_measures = midi_total_measures_for_ticks(d, measure_ticks);

    int total_lines = (total_measures + mpl - 1) / mpl;
    int pages = (total_lines + lines_per_page - 1) / lines_per_page;
    return pages < 1 ? 1 : pages;
}

/* ───────────────────────────────────────────────────
 *  midi_generate_line — 生成一行简谱文本
 *
 *  参数:
 *    d            — MIDI 数据
 *    buf / sz     — 输出缓冲区
 *    page         — 页码（0 开始）
 *    line_idx     — 行号（0 开始）
 *    lines_per_page — 每页行数
 *
 *  内部调用 midi_calc_measures_per_line 确保
 *  同一页所有行的小节数一致。
 * ─────────────────────────────────────────────────── */
bool midi_generate_line(const midi_data_t *d, char *buf, size_t sz,
                        int page, int line_idx, int lines_per_page)
{
    if (!d || !buf || sz == 0) return false;
    if (d->note_count == 0) { snprintf(buf, sz, " "); return true; }

    int mpl = midi_calc_measures_per_line(d);
    uint32_t measure_ticks_u32 = 0;
    if (!numbered_meter_ticks(d, NULL, &measure_ticks_u32) ||
        measure_ticks_u32 > INT_MAX)
        return false;
    int measure_ticks = (int)measure_ticks_u32;
    int total_measures = midi_total_measures_for_ticks(d, measure_ticks);

    int start_m = (page * lines_per_page + line_idx) * mpl;

    if (start_m >= total_measures) {
        buf[0] = '\0';
        return false;
    }

    int cnt = mpl;
    if (start_m + mpl > total_measures) {
        cnt = total_measures - start_m;
    }

    fill_measures(d, buf, sz, start_m, cnt, 0,
                  NULL, 0, NULL, NULL);
    return true;
}

bool midi_generate_measure_range_mapped(
    const midi_data_t *d,
    char *buf, size_t sz,
    int start_measure, int measure_count,
    uint8_t staff,
    midi_numbered_note_ref_t *refs, size_t ref_capacity,
    size_t *ref_count,
    bool *truncated)
{
    if (ref_count) *ref_count = 0;
    if (truncated) *truncated = false;
    if (!d || !buf || sz == 0 || start_measure < 0 || measure_count <= 0)
        return false;
    if ((refs == NULL && ref_capacity != 0) ||
        (refs != NULL && (ref_capacity == 0 || ref_count == NULL))) {
        return false;
    }
    if (d->note_count == 0) {
        snprintf(buf, sz, " ");
        return true;
    }

    uint32_t measure_ticks_u32 = 0;
    if (!numbered_meter_ticks(d, NULL, &measure_ticks_u32) ||
        measure_ticks_u32 > INT_MAX)
        return false;
    int measure_ticks = (int)measure_ticks_u32;
    int total_measures = midi_total_measures_for_ticks(d, measure_ticks);
    if (start_measure >= total_measures) {
        buf[0] = '\0';
        return false;
    }
    if (start_measure + measure_count > total_measures)
        measure_count = total_measures - start_measure;
    fill_measures(d, buf, sz, start_measure, measure_count,
                  staff == 2 ? 2 : staff == 1 ? 1 : 0,
                  refs, ref_capacity, ref_count, truncated);
    return true;
}

bool midi_generate_measure_range(const midi_data_t *d,
                                 char *buf, size_t sz,
                                 int start_measure, int measure_count,
                                 uint8_t staff)
{
    return midi_generate_measure_range_mapped(
        d, buf, sz, start_measure, measure_count, staff,
        NULL, 0, NULL, NULL);
}
