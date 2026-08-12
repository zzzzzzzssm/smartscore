#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCORE_CAPTURE_TASK_ID_CAPACITY 48U
#define SCORE_CAPTURE_MAX_JPEG_BYTES (14U * 1024U * 1024U)
#define SCORE_CAPTURE_MODEL_MAX_PIXELS 8388608ULL

typedef struct {
    const uint8_t *jpeg;
    size_t jpeg_length;
    uint32_t width;
    uint32_t height;
    uint64_t pixels;
    bool model_will_downscale;
    char task_id[SCORE_CAPTURE_TASK_ID_CAPACITY];
} score_capture_t;

/*
 * Creates a borrowed full-page JPEG view. The caller retains ownership and
 * must keep jpeg valid until the cloud request returns.
 */
esp_err_t score_capture_from_jpeg(const uint8_t *jpeg,
                                  size_t jpeg_length,
                                  const char *task_id,
                                  score_capture_t *out_capture);

#ifdef __cplusplus
}
#endif

