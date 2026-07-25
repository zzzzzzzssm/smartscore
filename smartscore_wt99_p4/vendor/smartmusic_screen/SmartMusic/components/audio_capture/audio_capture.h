#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 音频采集参数 */
#define AUDIO_CAPTURE_SAMPLE_RATE 48000
#define AUDIO_CAPTURE_FRAME_SAMPLES 512

/* 初始化麦克风（通过 BSP + ES7210 编解码器） */
esp_err_t audio_capture_init(void);

/* 读取一帧音频数据，返回实际样本数，输出为 int16_t */
int audio_capture_read_frame(int16_t *out_samples, int max_samples);

/* 计算去直流 RMS */
float audio_calculate_rms(const int16_t *samples, int n);

#ifdef __cplusplus
}
#endif
