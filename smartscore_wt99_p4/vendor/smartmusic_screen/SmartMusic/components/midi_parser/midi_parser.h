#ifndef MIDI_PARSER_H
#define MIDI_PARSER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MAX_NOTES 512

typedef struct {
    uint8_t  note;       /* MIDI note number 0-127 */
    uint8_t  velocity;   /* Velocity 0-127 */
    uint8_t  staff;      /* 0/1=upper staff, 2=lower piano staff */
    uint8_t  voice;      /* 0 defaults to voice 1 */
    uint32_t start_tick; /* Start tick */
    uint32_t duration;   /* Duration in ticks */
} midi_note_t;

typedef struct {
    int   tonality_sf;        /* Key signature sharps/flats (-7 to 7) */
    bool  tonality_minor;     /* true=minor, false=major */
    bool  tonality_forced;    /* 外部强制设置调性（跳过 MIDI 解析和自动检测） */
    int   bpm;                /* Beats per minute */
    int   ticks_per_quarter;  /* Ticks per quarter note from header */
    int   time_sig_num;       /* Time signature numerator (e.g. 4) */
    int   time_sig_den;       /* Time signature denominator power (2=quarter) */
    bool  time_sig_forced;    /* 外部强制设置拍号（跳过 MIDI 解析和自动检测） */
    char  title[24];          /* 曲名（最多 7 个中文字符） */
    int   note_count;         /* Number of notes parsed */
    midi_note_t notes[MAX_NOTES];
} midi_data_t;

/**
 * @brief 外部强制设置调性（跳过 MIDI FF 59 事件和自动检测）
 * @param d     MIDI 数据（在 midi_parse 之前调用）
 * @param sf    Key signature (-7~7, 0=C大调)
 * @param minor true=小调, false=大调
 */
void midi_set_tonality(midi_data_t *d, int sf, bool minor);

/**
 * @brief 外部强制设置拍号（跳过 MIDI FF 58 事件和自动检测）
 * @param d    MIDI 数据（在 midi_parse 之前调用）
 * @param num  拍号分子 (如 2,3,4)
 * @param den  拍号分母的幂 (2=四分音符, 3=八分音符)
 */
void midi_set_time_signature(midi_data_t *d, int num, int den);

/**
 * @brief 查询调性是否为外部强制设置
 */
bool midi_is_tonality_forced(const midi_data_t *d);

/**
 * @brief 查询拍号是否为外部强制设置
 */
bool midi_is_time_sig_forced(const midi_data_t *d);

/**
 * @brief 解析 MIDI 文件二进制数据
 * @param data MIDI 文件数据指针
 * @param len  数据长度
 * @param out  输出解析结果
 * @return true 解析成功
 */
bool midi_parse(const uint8_t *data, size_t len, midi_data_t *out);

/**
 * @brief 将 tonality 转换为可读字符串（如 "1=C 4/4"）
 */
void midi_tonality_to_string(int sf, bool minor, char *buf, size_t buf_size);

/**
 * @brief 将 BPM 转换为可读字符串（如 "BPM=120"）
 */
void midi_bpm_to_string(int bpm, char *buf, size_t buf_size);

/**
 * @brief 内容感知计算每行应放置的小节数
 *
 * 扫描全部小节的实际音符内容，基于 30 字符/行的限制
 * 自适应决定最优的每行小节数。一旦确定，所有行使用相同值。
 *
 * 字符估算 (SimpMusic 36px, 440px行宽≈33字符):
 *   四分/休止/延长: 2字符/拍 ("N " 或 "- ")
 *   两个八分音符:   3字符/拍 ("ab ")
 *   小节前缀:       2字符 ("| ")
 *
 * @param data 解析后的 MIDI 数据
 * @return 每行小节数 (1-6)
 */
int midi_calc_measures_per_line(const midi_data_t *data);

/**
 * @brief 生成一行简谱文本（SimpMusic 字体）
 *
 * 内部调用 midi_calc_measures_per_line 确保同页一致。
 *
 * @param data       解析后的 MIDI 数据
 * @param buf        输出缓冲区
 * @param buf_size   缓冲区大小
 * @param page       页码（从 0 开始）
 * @param line_idx   行号（0/1/2）
 * @param total_lines 总行数（3）
 * @return true 如果该行有内容
 */
bool midi_generate_line(const midi_data_t *data, char *buf, size_t buf_size,
                        int page, int line_idx, int total_lines);

/** Generate an explicit measure range, optionally filtering one piano staff.
 * staff=0 includes all notes; staff=1/2 selects the upper/lower staff. */
bool midi_generate_measure_range(const midi_data_t *data,
                                 char *buf, size_t buf_size,
                                 int start_measure, int measure_count,
                                 uint8_t staff);

/**
 * @brief 获取简谱总页数（每页 3 行）
 */
int midi_get_page_count(const midi_data_t *data, int lines_per_page);

/**
 * @brief 将 MIDI note 转换为简谱数字 (1-7) 和 八度标记
 * @param midi_note  MIDI note number (0-127)
 * @param sf         Key signature sharps/flats
 * @param number     输出简谱数字 (1-7)
 * @param octave     输出八度偏移（0=中音, >0 = 高音, <0 = 低音）
 */
void midi_note_to_jianpu(int midi_note, int sf, int *number, int *octave);

#endif /* MIDI_PARSER_H */
