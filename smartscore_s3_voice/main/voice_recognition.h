#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t voice_recognition_init(void);
size_t voice_recognition_get_feed_chunk_samples(void);
void voice_recognition_feed_task(void *arg);
void voice_recognition_detect_task(void *arg);
void voice_recognition_stop(void);

#ifdef __cplusplus
}
#endif
