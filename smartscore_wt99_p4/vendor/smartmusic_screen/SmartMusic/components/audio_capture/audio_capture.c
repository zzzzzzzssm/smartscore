#include "audio_capture.h"

#include <math.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "bsp/m5stack_tab5.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "audio_capture";

static esp_codec_dev_handle_t s_mic_dev = NULL;
static int16_t *s_read_buf = NULL;

/* ES7210 默认是 16bit 数据，我们直接用 int16_t 容器 */
#define READ_BUF_SAMPLES AUDIO_CAPTURE_FRAME_SAMPLES

esp_err_t audio_capture_init(void)
{
    if (s_mic_dev != NULL) {
        return ESP_OK;
    }

    /* 1. 初始化 I2C (BSP 依赖) */
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "bsp_i2c_init failed");

    /* 2. 初始化音频（I2S 全双工） */
    ESP_RETURN_ON_ERROR(bsp_audio_init(NULL), TAG, "bsp_audio_init failed");

    /* 3. 初始化麦克风编解码器 */
    s_mic_dev = bsp_audio_codec_microphone_init();
    if (s_mic_dev == NULL) {
        ESP_LOGE(TAG, "microphone init failed");
        return ESP_FAIL;
    }

    /* 4. 配置音频参数 */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = AUDIO_CAPTURE_SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_mic_dev, &fs), TAG, "codec open failed");

    /* 增益调到最大 37.5dB */
    esp_codec_dev_set_in_gain(s_mic_dev, 37.5);

    /* 5. 分配读取缓冲区 */
    s_read_buf = heap_caps_malloc(READ_BUF_SAMPLES * sizeof(int16_t), MALLOC_CAP_8BIT);
    if (s_read_buf == NULL) {
        ESP_LOGE(TAG, "no mem for read buf");
        esp_codec_dev_close(s_mic_dev);
        s_mic_dev = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ES7210 microphone ready: %d Hz, %d samples/frame",
             AUDIO_CAPTURE_SAMPLE_RATE, AUDIO_CAPTURE_FRAME_SAMPLES);
    return ESP_OK;
}

int audio_capture_read_frame(int16_t *out_samples, int max_samples)
{
    if (s_mic_dev == NULL || out_samples == NULL || max_samples <= 0) {
        return 0;
    }

    int requested = max_samples;
    if (requested > READ_BUF_SAMPLES) {
        requested = READ_BUF_SAMPLES;
    }

    memset(s_read_buf, 0, READ_BUF_SAMPLES * sizeof(int16_t));

    int ret = esp_codec_dev_read(s_mic_dev, s_read_buf,
                                 requested * sizeof(int16_t));
    if (ret != ESP_CODEC_DEV_OK) {
        return 0;
    }

    /* 成功时所有 requested 字节已读到 s_read_buf */
    int samples_read = requested;


    if (samples_read > requested) {
        samples_read = requested;
    }

    memcpy(out_samples, s_read_buf, samples_read * sizeof(int16_t));
    return samples_read;
}

float audio_calculate_rms(const int16_t *samples, int n)
{
    if (samples == NULL || n <= 0) {
        return 0.0f;
    }

    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        sum += (float)samples[i];
    }
    float mean = sum / (float)n;

    float square_sum = 0.0f;
    for (int i = 0; i < n; i++) {
        float v = (float)samples[i] - mean;
        square_sum += v * v;
    }
    return sqrtf(square_sum / (float)n);
}
