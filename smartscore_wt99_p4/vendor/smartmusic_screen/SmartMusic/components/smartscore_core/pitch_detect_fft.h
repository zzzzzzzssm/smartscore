#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FFT + Harmonic Product Spectrum (HPS) 音高检测
 *
 * 对输入的时域信号做 1024 点实 FFT，提取幅值谱，
 * 再用 HPS（将频谱按整数倍下采样后相乘）定位基频。
 * 对乐音（谐波丰富）场景比纯 YIN 更鲁棒。
 *
 * @param samples     PCM 16-bit 有符号采样
 * @param n           采样点数（建议 ≥ 1024）
 * @param sample_rate 采样率 (Hz)
 * @param confidence  [输出] 置信度 0~1，传 NULL 可忽略
 * @return 检测到的基频 (Hz)，失败返回 0.0f
 */
float pitch_detect_fft_hps(const int16_t *samples, int n, int sample_rate, float *confidence);

#ifdef __cplusplus
}
#endif
