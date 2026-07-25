#include "audio_inmp441.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "voice_config.h"

static const char *TAG = "audio_inmp441";

static i2s_chan_handle_t s_rx_channel;
static int32_t *s_raw_samples;
static size_t s_raw_capacity;
static bool s_channel_enabled;
static uint64_t s_rms_square_sum;
static uint32_t s_rms_sample_count;

static int16_t inmp441_raw_to_pcm(int32_t raw_sample)
{
    /*
     * The microphone's signed 24-bit value occupies bits 31..8. Converting
     * a full-scale 24-bit value to 16-bit therefore divides by 2^16. Apply
     * a small fixed digital gain in 64-bit precision before explicit clamp.
     */
    int64_t scaled = ((int64_t)raw_sample * VOICE_MIC_DIGITAL_GAIN) / (1LL << 16);
    if (scaled > INT16_MAX) {
        scaled = INT16_MAX;
    } else if (scaled < INT16_MIN) {
        scaled = INT16_MIN;
    }
    return (int16_t)scaled;
}

static void update_rms(const int16_t *pcm, size_t sample_count)
{
    for (size_t i = 0; i < sample_count; ++i) {
        const int32_t sample = pcm[i];
        s_rms_square_sum += (uint64_t)((int64_t)sample * sample);
        ++s_rms_sample_count;
    }

    if (s_rms_sample_count >= VOICE_SAMPLE_RATE_HZ) {
        const double mean_square = (double)s_rms_square_sum / (double)s_rms_sample_count;
        ESP_LOGI(TAG, "音频 RMS: %.1f", sqrt(mean_square));
        s_rms_square_sum = 0;
        s_rms_sample_count = 0;
    }
}

esp_err_t audio_inmp441_init(void)
{
    if (s_rx_channel != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(VOICE_I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&channel_config, NULL, &s_rx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "创建 I2S0 RX 通道失败: %s", esp_err_to_name(err));
        s_rx_channel = NULL;
        return err;
    }

    i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(VOICE_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = VOICE_I2S_BCLK_GPIO,
            .ws = VOICE_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = VOICE_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    err = i2s_channel_init_std_mode(s_rx_channel, &std_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置 I2S 标准 RX 模式失败: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx_channel);
        s_rx_channel = NULL;
        return err;
    }

    err = i2s_channel_enable(s_rx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动 I2S RX 通道失败: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx_channel);
        s_rx_channel = NULL;
        return err;
    }
    s_channel_enabled = true;

    ESP_LOGI(TAG, "I2S0 RX: 16 kHz, 32-bit slot, left channel, BCLK=%d WS=%d DIN=%d",
             VOICE_I2S_BCLK_GPIO, VOICE_I2S_WS_GPIO, VOICE_I2S_DIN_GPIO);
    return ESP_OK;
}

esp_err_t audio_inmp441_prepare_frame(size_t frame_samples)
{
    if (s_rx_channel == NULL || frame_samples == 0 ||
        frame_samples > (SIZE_MAX / sizeof(*s_raw_samples))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_raw_samples != NULL) {
        return (frame_samples == s_raw_capacity) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    s_raw_samples = heap_caps_calloc(
        frame_samples,
        sizeof(*s_raw_samples),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (s_raw_samples == NULL) {
        ESP_LOGE(TAG, "无法分配 %u 个 32 位 I2S 原始样本", (unsigned)frame_samples);
        return ESP_ERR_NO_MEM;
    }
    s_raw_capacity = frame_samples;
    return ESP_OK;
}

esp_err_t audio_inmp441_read_pcm(int16_t *pcm, size_t sample_count)
{
    if (pcm == NULL || s_rx_channel == NULL || s_raw_samples == NULL ||
        sample_count == 0 || sample_count > s_raw_capacity) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t required_bytes = sample_count * sizeof(*s_raw_samples);
    size_t total_bytes = 0;
    while (total_bytes < required_bytes) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(
            s_rx_channel,
            (uint8_t *)s_raw_samples + total_bytes,
            required_bytes - total_bytes,
            &bytes_read,
            VOICE_I2S_READ_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
        if (bytes_read == 0 || (bytes_read % sizeof(*s_raw_samples)) != 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        total_bytes += bytes_read;
    }

    for (size_t i = 0; i < sample_count; ++i) {
        pcm[i] = inmp441_raw_to_pcm(s_raw_samples[i]);
    }
    update_rms(pcm, sample_count);
    return ESP_OK;
}

void audio_inmp441_deinit(void)
{
    if (s_rx_channel != NULL) {
        if (s_channel_enabled) {
            (void)i2s_channel_disable(s_rx_channel);
            s_channel_enabled = false;
        }
        (void)i2s_del_channel(s_rx_channel);
        s_rx_channel = NULL;
    }

    free(s_raw_samples);
    s_raw_samples = NULL;
    s_raw_capacity = 0;
    s_rms_square_sum = 0;
    s_rms_sample_count = 0;
}
