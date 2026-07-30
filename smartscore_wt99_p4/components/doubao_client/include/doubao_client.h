#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AI 图片识谱接口的设备端内存上限。网页会先压缩图片，再把这个大小以内的数据发给 ESP32。 */
#define DOUBAO_MAX_IMAGE_BYTES (768 * 1024)

bool doubao_api_key_configured(void);

/*
 * 调用 Doubao-Seed-1.6-vision，将乐谱图片识别为 score.json 兼容结构。
 * 返回值是 malloc 出来的 JSON 字符串，调用者负责 free。
 */
char *doubao_recognize_sheet_image(const uint8_t *image,
                                   size_t image_len,
                                   const char *mime_type);

/*
 * 调用 Doubao-Seed-1.6-vision，根据最近一次评分结果生成 AI 评分和练习建议。
 * context_json 是评分服务结果的副本，返回值同样需要调用者 free。
 */
char *doubao_score_performance(const char *context_json);

#ifdef __cplusplus
}
#endif

