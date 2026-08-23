#include "esp_err.h"
#include "esp_log.h"
#include "board_pins.h"
#include "music_detector.h"
#include "music_detector_config.h"
#include "music_uart_link.h"

void app_main(void)
{
    ESP_LOGI("MUSIC_MAIN", "ESP32-S3 IM68A130 music detector starting");
    ESP_LOGI("MUSIC_MAIN", "reference tuning: A4=%.1f Hz", MUSIC_REFERENCE_A4_HZ);
#if MUSIC_USE_SINGLE_MIC_CH1
    ESP_LOGI("MUSIC_MAIN", "microphone mode: SINGLE CH1");
    ESP_LOGI("MUSIC_MAIN", "active microphone: IM68A130 on ES7210 MIC1");
    ESP_LOGI("MUSIC_MAIN", "MIC2 disabled/ignored");
    ESP_LOGI("AUDIO", "MIC1 slot index=%d", BOARD_MIC1_SLOT_INDEX);
    ESP_LOGI("AUDIO", "sample_rate=%d", MUSIC_SAMPLE_RATE_HZ);
    ESP_LOGI("AUDIO", "input_gain=%.1f dB", MUSIC_ES7210_INPUT_GAIN_DB);
#else
    ESP_LOGI("MUSIC_MAIN", "microphones: 2 x IM68A130 analog single-ended modules");
    ESP_LOGI("MUSIC_MAIN", "microphone mode: DUAL quality-selected (no unaligned time-domain mixing)");
    ESP_LOGI("AUDIO", "nominal MIC1 slot=%d MIC2 slot=%d; startup isolation probe enabled",
             BOARD_MIC1_SLOT_INDEX, BOARD_MIC2_SLOT_INDEX);
    ESP_LOGI("AUDIO", "sample_rate=%d input_gain=%.1f dB",
             MUSIC_SAMPLE_RATE_HZ, MUSIC_ES7210_INPUT_GAIN_DB);
#endif
    const esp_err_t link_error = music_uart_link_init();
    if (link_error != ESP_OK) {
        ESP_LOGE("MUSIC_MAIN", "music UART link initialization failed: %s",
                 esp_err_to_name(link_error));
        return;
    }
    const esp_err_t error = music_detector_start();
    music_uart_link_set_ready(error == ESP_OK);
    if (error != ESP_OK) {
        ESP_LOGE("MUSIC_MAIN", "startup aborted: %s; no recognition task is running",
                 esp_err_to_name(error));
    }
}
