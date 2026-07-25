#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "midi_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Save a completed MIDI snapshot to SD with an automatic title. */
esp_err_t score_storage_save_midi_auto(const midi_data_t *score,
                                       char *saved_title, size_t title_size,
                                       char *saved_filename,
                                       size_t filename_size);

#define SCORE_LIST_MAX 50
#define SCORE_STORAGE_MAX_FILE_BYTES (256 * 1024)

typedef struct {
    char filename[64];
    char title[64];
    char key[16];
    char time_signature[8];
    int  bpm;
    int  note_count;
    int  measure_count;
    size_t file_size;
} score_info_t;

/**
 * @brief 挂载 SPIFFS 并扫描本地乐谱 JSON 文件
 * @param out       输出数组
 * @param max_count 数组容量
 * @return 找到的乐谱数量，< 0 表示错误
 */
int score_storage_scan(score_info_t *out, int max_count);

/**
 * Scan only valid JSON scores from /sdcard/scores.
 *
 * Returns the number of entries, or -1 when the SD score directory is not
 * available. Unlike score_storage_scan(), this does not include the built-in
 * test score or SPIFFS files.
 */
int score_storage_scan_sd(score_info_t *out, int max_count);

/** Validate one safe, single-component SD score JSON filename. */
bool score_storage_is_valid_sd_filename(const char *filename);

/**
 * @brief 读取指定乐谱文件并解析存储到 app_state
 * @param filename 文件名（不含路径）
 * @return true 成功
 */
bool score_storage_load(const char *filename);

/** Load one score strictly from /sdcard/scores into the shared score store. */
bool score_storage_load_sd(const char *filename);

/** Load a score and scale its target timing to an optional practice tempo. */
bool score_storage_load_with_tempo(const char *filename, int tempo_bpm);

/** Read one local score as a NUL-terminated heap buffer; caller frees it. */
bool score_storage_read_json(const char *filename, char **json, size_t *size);

/** Read one JSON score strictly from /sdcard/scores; caller frees the buffer. */
bool score_storage_read_sd_json(const char *filename, char **json, size_t *size);

/** Replace only the title field of one SD score JSON file. */
esp_err_t score_storage_rename_sd_title(const char *filename,
                                        const char *title);

/**
 * @brief 确保 SPIFFS 已挂载（幂等）
 */
esp_err_t score_storage_ensure_mounted(void);

#ifdef __cplusplus
}
#endif
