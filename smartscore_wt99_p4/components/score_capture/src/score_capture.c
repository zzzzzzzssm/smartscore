#include "score_capture.h"

#include <limits.h>
#include <string.h>

#include "driver/jpeg_decode.h"

esp_err_t score_capture_from_jpeg(const uint8_t *jpeg,
                                  size_t jpeg_length,
                                  const char *task_id,
                                  score_capture_t *out_capture)
{
    if (jpeg == NULL || jpeg_length < 4U || task_id == NULL ||
        task_id[0] == '\0' || out_capture == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(task_id) >= sizeof(out_capture->task_id) ||
        jpeg_length > SCORE_CAPTURE_MAX_JPEG_BYTES ||
        jpeg_length > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (jpeg[0] != 0xffU || jpeg[1] != 0xd8U) {
        return ESP_ERR_INVALID_ARG;
    }

    jpeg_decode_picture_info_t info = {0};
    esp_err_t error = jpeg_decoder_get_info(jpeg, (uint32_t)jpeg_length,
                                             &info);
    if (error != ESP_OK || info.width <= 10U || info.height <= 10U) {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t pixels = (uint64_t)info.width * (uint64_t)info.height;
    if (pixels == 0U ||
        (uint64_t)info.width > (uint64_t)info.height * 200U ||
        (uint64_t)info.height > (uint64_t)info.width * 200U) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_capture, 0, sizeof(*out_capture));
    out_capture->jpeg = jpeg;
    out_capture->jpeg_length = jpeg_length;
    out_capture->width = info.width;
    out_capture->height = info.height;
    out_capture->pixels = pixels;
    out_capture->model_will_downscale =
        pixels > SCORE_CAPTURE_MODEL_MAX_PIXELS;
    strlcpy(out_capture->task_id, task_id, sizeof(out_capture->task_id));
    return ESP_OK;
}
