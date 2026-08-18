#include "wake_prompt.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board_sdcard.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "speaker_service.h"

#define WAKE_PROMPT_FILE_PATH \
    BOARD_SDCARD_MOUNT_POINT "/SmartMusic/system/wake_prompt.wav"
#define WAKE_PROMPT_TASK_STACK_BYTES 4096U
#define WAKE_PROMPT_TASK_PRIORITY 5U
#define WAKE_PROMPT_START_TIMEOUT_MS 500U
#define WAKE_PROMPT_PCM_SAMPLES 768U

static const char *TAG = "WAKE_PROMPT";
static TaskHandle_t s_task;
static int16_t s_pcm[WAKE_PROMPT_PCM_SAMPLES];

static uint16_t read_u16(const uint8_t *value)
{
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8U);
}

static uint32_t read_u32(const uint8_t *value)
{
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8U) |
           ((uint32_t)value[2] << 16U) | ((uint32_t)value[3] << 24U);
}

static esp_err_t open_prompt(FILE **out_file,
                             uint32_t *out_sample_rate,
                             uint32_t *out_data_bytes)
{
    if (out_file == NULL || out_sample_rate == NULL ||
        out_data_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = board_sdcard_mount();
    if (err != ESP_OK) return err;

    FILE *file = fopen(WAKE_PROMPT_FILE_PATH, "rb");
    if (file == NULL) return ESP_ERR_NOT_FOUND;

    uint8_t riff[12];
    if (fread(riff, 1, sizeof(riff), file) != sizeof(riff) ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool format_found = false;
    uint16_t format = 0U;
    uint16_t channels = 0U;
    uint16_t bits = 0U;
    uint32_t sample_rate = 0U;
    uint32_t data_bytes = 0U;
    for (unsigned chunk = 0U; chunk < 32U; ++chunk) {
        uint8_t header[8];
        if (fread(header, 1, sizeof(header), file) != sizeof(header)) break;
        const uint32_t size = read_u32(header + 4);
        if (memcmp(header, "fmt ", 4) == 0) {
            if (size < 16U || size > 128U) break;
            uint8_t details[128];
            if (fread(details, 1, size, file) != size) break;
            format = read_u16(details);
            channels = read_u16(details + 2);
            sample_rate = read_u32(details + 4);
            bits = read_u16(details + 14);
            format_found = true;
            if ((size & 1U) != 0U && fseek(file, 1L, SEEK_CUR) != 0) break;
        } else if (memcmp(header, "data", 4) == 0) {
            if (!format_found) break;
            data_bytes = size;
            if (format == 1U && channels == 1U && bits == 16U &&
                (sample_rate == 16000U || sample_rate == 24000U) &&
                (data_bytes & 1U) == 0U) {
                *out_file = file;
                *out_sample_rate = sample_rate;
                *out_data_bytes = data_bytes;
                return ESP_OK;
            }
            break;
        } else if (fseek(file, (long)(size + (size & 1U)), SEEK_CUR) != 0) {
            break;
        }
    }

    fclose(file);
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t wait_for_held_stream(void)
{
    const TickType_t deadline = xTaskGetTickCount() +
                                pdMS_TO_TICKS(WAKE_PROMPT_START_TIMEOUT_MS);
    do {
        speaker_stream_metrics_t metrics;
        speaker_service_stream_get_metrics(&metrics);
        if (metrics.active && metrics.held) return ESP_OK;
        vTaskDelay(1);
    } while ((int32_t)(deadline - xTaskGetTickCount()) > 0);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t queue_prompt_pcm(FILE *file, uint32_t data_bytes)
{
    uint32_t remaining = data_bytes;
    while (remaining > 0U) {
        size_t requested = remaining < sizeof(s_pcm)
                               ? remaining
                               : sizeof(s_pcm);
        requested &= ~(size_t)1U;
        if (fread(s_pcm, 1, requested, file) != requested)
            return ESP_ERR_INVALID_SIZE;
        remaining -= (uint32_t)requested;

        size_t offset = 0U;
        const size_t samples = requested / sizeof(s_pcm[0]);
        while (offset < samples) {
            size_t accepted = 0U;
            esp_err_t err = speaker_service_stream_write(
                s_pcm + offset, samples - offset, &accepted);
            offset += accepted;
            if (offset < samples) {
                if (err != ESP_OK && err != ESP_ERR_TIMEOUT) return err;
                vTaskDelay(1);
            }
        }
    }
    return ESP_OK;
}

static void play_prompt_once(void)
{
    FILE *file = NULL;
    uint32_t sample_rate = 0U;
    uint32_t data_bytes = 0U;
    esp_err_t err = open_prompt(&file, &sample_rate, &data_bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wake WAV unavailable: %s (%s)",
                 WAKE_PROMPT_FILE_PATH, esp_err_to_name(err));
        return;
    }

    bool stream_owned = false;
    err = speaker_service_stream_start_held(sample_rate);
    if (err == ESP_OK) {
        stream_owned = true;
        err = wait_for_held_stream();
    }
    if (err == ESP_OK) err = queue_prompt_pcm(file, data_bytes);
    fclose(file);

    if (err == ESP_OK) err = speaker_service_stream_release();
    if (err == ESP_OK) err = speaker_service_stream_finish();
    if (err != ESP_OK) {
        if (stream_owned) (void)speaker_service_stream_abort();
        ESP_LOGW(TAG, "wake WAV playback failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "wake WAV queued: %uHz %u bytes",
             (unsigned)sample_rate, (unsigned)data_bytes);
}

static void wake_prompt_task(void *argument)
{
    (void)argument;
    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        play_prompt_once();
    }
}

esp_err_t wake_prompt_init(void)
{
    if (s_task != NULL) return ESP_OK;
    return xTaskCreate(wake_prompt_task, "wake_prompt",
                       WAKE_PROMPT_TASK_STACK_BYTES, NULL,
                       WAKE_PROMPT_TASK_PRIORITY, &s_task) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

esp_err_t wake_prompt_play(void)
{
    if (s_task == NULL || !speaker_service_is_ready())
        return ESP_ERR_INVALID_STATE;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}
