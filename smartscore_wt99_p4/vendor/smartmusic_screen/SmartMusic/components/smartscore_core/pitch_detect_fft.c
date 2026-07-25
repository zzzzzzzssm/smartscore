/**
 * @file pitch_detect_fft.c
 * @brief FFT + Harmonic Product Spectrum (HPS) 音高检测
 *
 * 流程:
 *   1. 将输入 PCM 数据拷贝到 gmf_fft 缓冲区（补零到 1024 点）
 *   2. 应用汉宁窗 (Hann window) 减少频谱泄漏
 *   3. 调用 gmf_fft 定点实数 FFT
 *   4. 提取幅值谱: |X[k]| ≈ |Re| + |Im|  (避免 sqrt)
 *   5. HPS: 对候选基频 bin，将其 1~H 次谐波的幅值相乘
 *   6. 抛物线插值提高频率精度，转换到 MIDI
 *
 * 本模块所有核心运算均使用定点整数，仅最终插值用 float。
 */

#include "pitch_detect_fft.h"

#include <math.h>
#include <string.h>
#include "esp_err.h"
#include "esp_gmf_fft.h"
#include "esp_log.h"

static const char *TAG = "pitch_detect_fft";

/* ---------- 配置参数 ---------- */

/** FFT 点数（2 的幂，gmf_fft 支持 32~8192） */
#define FFT_SIZE      1024

/** HPS 最多考虑多少次谐波 */
#define HPS_N_HARM    5

/** 注意力范围 (Hz) - 与 YIN 保持一致 */
#define HPS_MIN_HZ    80.0f
#define HPS_MAX_HZ    1200.0f

/** 检测到基频后检查半频：若半频处幅值更大则取半频 */
#define HPS_OCTAVE_CORRECT 1

/** b 越大对谐波越敏感，但对噪声也更敏感 */
#define HPS_EXPONENT  1.0f

/* ---------- 静态变量 ---------- */

/** gmf_fft handle（全局复用） */
static esp_gmf_fft_handle_t s_fft = NULL;

/** FFT 数据缓冲区（gmf_fft 内部使用） */
static int16_t s_fft_buf[ESP_GMF_FFT_BUFFER_SIZE(FFT_SIZE)];

/** 幅值谱缓冲区 (N/2 + 1 个 bin) */
static uint16_t s_mag[FFT_SIZE / 2 + 1];

/** 初始化标志 */
static bool s_inited = false;

/* ---------- 汉宁窗（定点 Q15） ---------- */

/**
 * @brief 就地应用汉宁窗 (Q15 定点)
 *
 * 窗系数预先计算为 Q15 格式，用 (x * w) >> 15 实现乘法。
 * 如果只用一次就不预先计算了，用浮点生成然后转 Q15。
 */
static void apply_hann_q15(int16_t *buf, int n)
{
    /* 汉宁窗: w[i] = 0.5 * (1 - cos(2π * i / (n-1)))
     * Q15 范围 -32768~32767（对应 -1.0~+0.99997），
     * 所以 w=1.0 应映射到 32767，而非 32768（会溢出为 -32768）。
     */
    for (int i = 0; i < n; i++) {
        float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (n - 1)));
        int16_t w_q15 = (int16_t)(w * 32767.0f + 0.5f);
        buf[i] = (int16_t)(((int32_t)buf[i] * w_q15) >> 15);
    }
}

/* ---------- 幅值谱 ---------- */

/**
 * @brief 从 gmf_fft 输出提取幅值谱
 *
 * gmf_fft 实数 FFT 输出布局:
 *   data[0]   = DC 实部
 *   data[1]   = Nyquist 实部
 *   data[2k]   = bin k 实部 (k = 1 .. N/2-1)
 *   data[2k+1] = bin k 虚部 (k = 1 .. N/2-1)
 *
 * 幅值近似: |X| ≈ |Re| + |Im|  (避免开方)
 */
static void extract_magnitudes(const int16_t *fft_out, uint16_t *mag, int n_bins)
{
    /* bin 0: DC */
    mag[0] = (uint16_t)(fft_out[0] < 0 ? -fft_out[0] : fft_out[0]);

    /* bin 1 .. N/2-1 */
    for (int k = 1; k < n_bins - 1; k++) {
        int re = fft_out[2 * k];
        int im = fft_out[2 * k + 1];
        int abs_re = re < 0 ? -re : re;
        int abs_im = im < 0 ? -im : im;
        int approx = abs_re + abs_im;
        mag[k] = approx > 65535 ? 65535 : (uint16_t)approx;
    }

    /* bin N/2: Nyquist */
    mag[n_bins - 1] = (uint16_t)(fft_out[1] < 0 ? -fft_out[1] : fft_out[1]);
}

/* ---------- HPS 核心 ---------- */

/**
 * @brief 在幅值谱上执行 HPS，返回最佳基频 (Hz)
 *
 * 对每个候选 bin f，计算:
 *   P(f) = ∏_{h=1}^{H} M(f * h)
 * 其中 M(·) 为线性插值后的幅值。
 * 取 P(f) 最大的 f 作为基频 bin，再做抛物线插值。
 */
/**
 * @brief 在对数域计算 HPS 得分，避免连乘溢出和精度丢失
 *
 * score(f) = Σ log(mag[f * h])   for h = 1..H
 * 由于 log 单调，score 最大的 bin 即最优基频。
 */
static float hps_pitch(const uint16_t *mag, int n_bins,
                        float bin_res_hz, float *out_confidence)
{
    int min_bin = (int)(HPS_MIN_HZ / bin_res_hz);
    int max_bin = (int)(HPS_MAX_HZ / bin_res_hz);
    if (min_bin < 1) min_bin = 1;
    if (max_bin >= n_bins) max_bin = n_bins - 1;
    if (max_bin <= min_bin) return 0.0f;

    float best_score = -1e30f;
    float second_score = -1e30f;
    int best_bin = 0;

    for (int f = min_bin; f <= max_bin; f++) {
        float base = (float)mag[f];
        if (base < 10.0f) continue;

        float score = logf(base + 1.0f);

        for (int h = 2; h <= HPS_N_HARM; h++) {
            float harmonic_pos = (float)(f * h);
            if (harmonic_pos >= (float)(n_bins - 1)) break;

            int hi = (int)harmonic_pos;
            float frac = harmonic_pos - (float)hi;
            float hv = (float)mag[hi];
            if (hi + 1 < n_bins)
                hv += frac * ((float)mag[hi + 1] - (float)mag[hi]);
            score += logf(hv + 1.0f);
        }

        if (score > best_score) {
            second_score = best_score;
            best_score = score;
            best_bin = f;
        } else if (score > second_score) {
            second_score = score;
        }
    }

    if (best_bin <= 0) {
        if (out_confidence) *out_confidence = 0.0f;
        return 0.0f;
    }

    /* 抛物线插值（在幅值谱上做，而非得分） */
    float freq = (float)best_bin * bin_res_hz;
    if (best_bin > min_bin && best_bin < max_bin) {
        float y1 = (float)mag[best_bin - 1];
        float y2 = (float)mag[best_bin];
        float y3 = (float)mag[best_bin + 1];
        float denom = y1 + y3 - 2.0f * y2;
        if (fabsf(denom) > 1e-12f) {
            float shift = 0.5f * (y1 - y3) / denom;
            if (shift > -1.0f && shift < 1.0f)
                freq = ((float)best_bin + shift) * bin_res_hz;
        }
    }

    if (freq < HPS_MIN_HZ || freq > HPS_MAX_HZ) {
        if (out_confidence) *out_confidence = 0.0f;
        return 0.0f;
    }

    /* 八度修正：检查半频和 1/3 频 */
#if HPS_OCTAVE_CORRECT
    /* 在半频附近 0.5 bin 范围内搜索幅值峰值 */
    for (int div = 2; div <= 3; div++) {
        int low_bin = best_bin / div;
        if (low_bin < min_bin) continue;
        /* 找 low_bin ±1 范围内最大幅值 */
        int search_start = (low_bin - 1 > min_bin) ? low_bin - 1 : min_bin;
        int search_end = (low_bin + 1 < max_bin) ? low_bin + 1 : max_bin;
        float peak_mag = 0.0f;
        int peak_bin = 0;
        for (int s = search_start; s <= search_end; s++) {
            if ((float)mag[s] > peak_mag) {
                peak_mag = (float)mag[s];
                peak_bin = s;
            }
        }
        if (peak_bin > 0 && peak_mag * 0.8f > (float)mag[best_bin]) {
            /* 低八度/低三度的峰值显著 → 改用低音 */
            freq = (float)peak_bin * bin_res_hz;
            if (peak_bin > min_bin && peak_bin < max_bin) {
                float y1 = (float)mag[peak_bin - 1];
                float y2 = (float)mag[peak_bin];
                float y3 = (float)mag[peak_bin + 1];
                float denom = y1 + y3 - 2.0f * y2;
                if (fabsf(denom) > 1e-12f) {
                    float shift = 0.5f * (y1 - y3) / denom;
                    if (shift > -1.0f && shift < 1.0f)
                        freq = ((float)peak_bin + shift) * bin_res_hz;
                }
            }
            break; /* 只做一次修正 */
        }
    }
#endif

    /* 置信度：基于对数得分差距 */
    if (out_confidence) {
        float diff = best_score - second_score;
        /* diff 越大越确定。典型值：谐波丰富时约 5~20，噪声时 < 1 */
        float conf = diff / (diff + 3.0f);
        if (conf < 0.0f) conf = 0.0f;
        if (conf > 0.95f) conf = 0.95f;
        *out_confidence = conf;
    }

    return freq;
}

/* ---------- 公开 API ---------- */

float pitch_detect_fft_hps(const int16_t *samples, int n, int sample_rate, float *confidence)
{
    if (confidence) *confidence = 0.0f;
    if (samples == NULL || n < 64 || sample_rate <= 0) {
        return 0.0f;
    }

    if (!s_inited) {
        const esp_gmf_fft_cfg_t cfg = {
            .n_fft = FFT_SIZE,
            .fft_type = ESP_GMF_FFT_TYPE_REAL_Q15,
        };
        esp_err_t ret = esp_gmf_fft_init(&cfg, &s_fft);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "gmf_fft init failed: %d", ret);
            return 0.0f;
        }
        s_inited = true;
    }

    /* 1. 准备缓冲区 */
    int copy_n = (n < FFT_SIZE) ? n : FFT_SIZE;
    memset(s_fft_buf, 0, sizeof(s_fft_buf));
    memcpy(s_fft_buf, samples, copy_n * sizeof(int16_t));

    /* 2. 加汉宁窗 */
    apply_hann_q15(s_fft_buf, copy_n);

    /* 3. 执行实数 FFT */
    esp_err_t err = esp_gmf_fft_forward(s_fft, s_fft_buf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "FFT forward failed: %d", err);
        return 0.0f;
    }

    /* 4. 提取幅值谱 */
    int n_bins = FFT_SIZE / 2 + 1;
    extract_magnitudes(s_fft_buf, s_mag, n_bins);

    /* 5. HPS 检测 */
    float bin_res = (float)sample_rate / (float)FFT_SIZE;
    float freq = hps_pitch(s_mag, n_bins, bin_res, confidence);

    return freq;
}
