#include <inttypes.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_inmp441.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "voice_config.h"
#include "voice_recognition.h"
#include "voice_uart_link.h"

static const char *TAG = "app_main";

static esp_err_t check_board_memory(void)
{
    uint32_t flash_size = 0;
    esp_err_t err = esp_flash_get_size(NULL, &flash_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取 Flash 容量失败: %s", esp_err_to_name(err));
        return err;
    }
    if (flash_size < VOICE_EXPECTED_FLASH_BYTES) {
        ESP_LOGE(TAG, "Flash 容量不足: 检测到 %" PRIu32 " 字节，要求至少 %u 字节",
                 flash_size, VOICE_EXPECTED_FLASH_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM 未初始化，请检查 ESP32-S3-N16R8 配置");
        return ESP_ERR_INVALID_STATE;
    }

    const size_t psram_size = esp_psram_get_size();
    if (psram_size < VOICE_EXPECTED_PSRAM_BYTES) {
        ESP_LOGE(TAG, "PSRAM 容量不足: 检测到 %u 字节，要求至少 %u 字节",
                 (unsigned)psram_size, VOICE_EXPECTED_PSRAM_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Flash: %" PRIu32 " MB, PSRAM: %u MB",
             flash_size / (1024U * 1024U),
             (unsigned)(psram_size / (1024U * 1024U)));
    return ESP_OK;
}

void app_main(void)
{
    esp_err_t err = check_board_memory();
    if (err != ESP_OK) {
        return;
    }

    err = voice_uart_link_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "语音控制 UART 初始化失败: %s", esp_err_to_name(err));
        return;
    }

    err = audio_inmp441_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "INMP441 初始化失败: %s", esp_err_to_name(err));
        return;
    }

    err = voice_recognition_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP-SR 初始化失败: %s", esp_err_to_name(err));
        audio_inmp441_deinit();
        return;
    }

    const size_t feed_samples = voice_recognition_get_feed_chunk_samples();
    err = audio_inmp441_prepare_frame(feed_samples);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "麦克风帧缓冲初始化失败: %s", esp_err_to_name(err));
        voice_recognition_stop();
        audio_inmp441_deinit();
        return;
    }

    TaskHandle_t feed_task = NULL;
    BaseType_t task_result = xTaskCreatePinnedToCore(
        voice_recognition_feed_task,
        "voice_feed",
        VOICE_FEED_TASK_STACK_SIZE,
        NULL,
        VOICE_TASK_PRIORITY,
        &feed_task,
        VOICE_FEED_TASK_CORE);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "创建音频采集任务失败");
        voice_recognition_stop();
        audio_inmp441_deinit();
        return;
    }

    TaskHandle_t detect_task = NULL;
    task_result = xTaskCreatePinnedToCore(
        voice_recognition_detect_task,
        "voice_detect",
        VOICE_DETECT_TASK_STACK_SIZE,
        NULL,
        VOICE_TASK_PRIORITY,
        &detect_task,
        VOICE_DETECT_TASK_CORE);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "创建语音识别任务失败");
        voice_recognition_stop();
        return;
    }

    ESP_LOGI(TAG, "离线语音识别已启动，等待唤醒词“%s”", VOICE_WAKE_WORD_TEXT);
}
