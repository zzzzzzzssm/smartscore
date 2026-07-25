#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_event.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "bsp/esp-bsp.h"
#include "app_font.h"
#include "creator_mode.h"
#include "app_state.h"
#include "events_init.h"
#include "gui_guider.h"
#include "music_display.h"
#include "network_manager.h"
#include "score_storage.h"
#include "score_ui_flow.h"
#include "staff_display_test.h"
#include "usb_midi_input.h"
#include "wifi_manager.h"

static const char *TAG = "main";

#define HOSTED_TRANSPORT_UP_BIT BIT0
#define NETWORK_START_RETRY_MS  5000

static EventGroupHandle_t s_hosted_events;
static lv_font_t s_other_mode_font;

lv_ui guider_ui;

/* The WT99P4C5-S1 BSP owns the LVGL tick and handler task. */
static void display_init(void)
{
    lv_display_t *disp = bsp_display_start();
    assert(disp != NULL);

    /* WT99 backlight is GPIO20 LEDC PWM; start at the full 10-bit duty cycle. */
    ESP_ERROR_CHECK(bsp_display_brightness_set(100));
    ESP_LOGI(TAG, "Display + LVGL ready: %dx%d (native landscape)",
             BSP_LCD_H_RES, BSP_LCD_V_RES);
}

static void apply_main_menu_fonts(void)
{
    if (!guider_ui.screen_choose_button_label) return;

    /* Keep the generated classical font as the primary face. Its subset does
     * not contain every label glyph, so use the full SD CJK font as fallback. */
    s_other_mode_font = lv_font_gudianChinese_34;
    const lv_font_t *fallback = app_font_chinese_22();
    if (fallback) s_other_mode_font.fallback = fallback;
    lv_obj_set_style_text_font(guider_ui.screen_choose_button_label,
                               &s_other_mode_font, LV_PART_MAIN);
}

static bool start_selected_score(const char *filename,
                                 const score_practice_options_t *options,
                                 void *user_data)
{
    (void)user_data;
    if (!filename || !options) return false;

    char *json = NULL;
    size_t json_size = 0;
    if (!score_storage_read_json(filename, &json, &json_size)) {
        ESP_LOGE(TAG, "failed to read score: %s", filename);
        return false;
    }

    music_display_set_creator_active(false);
    music_display_set_read_only(options->read_only);
    if (options->read_only) app_state_stop_recording();

    int practice_bpm = options->metronome_enabled ?
                       options->metronome_bpm : options->score_bpm;
    bool state_ready = options->read_only ||
        score_storage_load_with_tempo(filename, practice_bpm);
    music_display_score_options_t display_options = {
        .tempo_bpm = options->score_bpm,
        .time_sig_num = options->score_time_sig_num,
        .time_sig_den = options->score_time_sig_den,
        .notation_type = options->notation_type == SCORE_NOTATION_NUMBERED ?
                         MUSIC_DISPLAY_NOTATION_NUMBERED :
                         MUSIC_DISPLAY_NOTATION_STAFF,
    };
    bool display_ready = state_ready &&
        music_display_apply_score_json_with_options(json, &display_options);
    free(json);

    if (!display_ready) {
        ESP_LOGE(TAG, "failed to prepare score session: %s", filename);
        if (options->read_only) music_display_set_read_only(false);
        return false;
    }

    if (options->read_only) {
        ESP_LOGI(TAG, "score opened read-only: %s", filename);
        return true;
    }

    esp_err_t err = app_state_start_recording();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start score session: %s",
                 esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG,
             "score started: %s, score=%d BPM %d/%d, practice=%d BPM, metronome=%s %d/%d, input=%s, notation=%s",
             filename, options->score_bpm, options->score_time_sig_num,
             options->score_time_sig_den, practice_bpm,
             options->metronome_enabled ? "on" : "off",
             options->metronome_time_sig_num,
             options->metronome_time_sig_den,
             options->input_source == SCORE_INPUT_USB_MIDI ? "usb-midi" :
                                                             "microphone",
             options->notation_type == SCORE_NOTATION_NUMBERED ?
                 "numbered" : "staff");
    return true;
}

static void on_animation_test_button_click(lv_event_t *event)
{
    (void)event;
    ESP_LOGI(TAG, "staff animation test button clicked");
    staff_display_test_show();
}

static bool hosted_sdio_config_is_valid(void)
{
#if CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE && \
    CONFIG_ESP_HOSTED_SDIO_PIN_CLK == 18 && \
    CONFIG_ESP_HOSTED_SDIO_PIN_CMD == 19 && \
    CONFIG_ESP_HOSTED_SDIO_PIN_D0 == 14 && \
    CONFIG_ESP_HOSTED_SDIO_PIN_D1 == 15 && \
    CONFIG_ESP_HOSTED_SDIO_PIN_D2 == 16 && \
    CONFIG_ESP_HOSTED_SDIO_PIN_D3 == 17 && \
    CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE == 54
    return true;
#else
    ESP_LOGE(TAG, "Invalid WT99 ESP-Hosted SDIO configuration");
#if CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE
    ESP_LOGE(TAG, "actual: CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d RESET=%d",
             CONFIG_ESP_HOSTED_SDIO_PIN_CLK,
             CONFIG_ESP_HOSTED_SDIO_PIN_CMD,
             CONFIG_ESP_HOSTED_SDIO_PIN_D0,
             CONFIG_ESP_HOSTED_SDIO_PIN_D1,
             CONFIG_ESP_HOSTED_SDIO_PIN_D2,
             CONFIG_ESP_HOSTED_SDIO_PIN_D3,
             CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE);
#endif
    ESP_LOGE(TAG, "expected: CLK=18 CMD=19 D0=14 D1=15 D2=16 D3=17 RESET=54");
    return false;
#endif
}

static void hosted_event_handler(void *arg, esp_event_base_t base,
                                 int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_data;

    if (event_id == ESP_HOSTED_EVENT_TRANSPORT_UP) {
        ESP_LOGI(TAG, "ESP-Hosted transport is up");
        xEventGroupSetBits(s_hosted_events, HOSTED_TRANSPORT_UP_BIT);
    } else if (event_id == ESP_HOSTED_EVENT_TRANSPORT_DOWN ||
               event_id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE) {
        ESP_LOGW(TAG, "ESP-Hosted transport unavailable (event=%ld)",
                 (long)event_id);
        xEventGroupClearBits(s_hosted_events, HOSTED_TRANSPORT_UP_BIT);
    }
}

static void network_start_task(void *arg)
{
    (void)arg;

    for (;;) {
        ESP_LOGI(TAG, "waiting for ESP-Hosted transport...");
        xEventGroupWaitBits(s_hosted_events, HOSTED_TRANSPORT_UP_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);

        esp_err_t err = network_manager_start();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "network manager started");
            break;
        }

        ESP_LOGE(TAG, "network start failed: %s; retrying in %d ms",
                 esp_err_to_name(err), NETWORK_START_RETRY_MS);
        vTaskDelay(pdMS_TO_TICKS(NETWORK_START_RETRY_MS));
    }

    vTaskDelete(NULL);
}

static void usb_midi_staff_bridge_task(void *arg)
{
    QueueHandle_t queue = (QueueHandle_t)arg;
    usb_midi_input_event_t event;

    for (;;) {
        if (xQueueReceive(queue, &event, portMAX_DELAY) != pdTRUE) continue;
        if (creator_mode_handle_midi_event(&event))
            continue;
        switch (event.type) {
        case USB_MIDI_INPUT_DEVICE_CONNECTED:
            ESP_LOGI(TAG, "USB MIDI keyboard ready: %04x:%04x %s",
                     event.vid, event.pid,
                     event.product[0] ? event.product : "unknown");
            staff_display_test_midi_connected();
            break;
        case USB_MIDI_INPUT_DEVICE_DISCONNECTED:
            ESP_LOGI(TAG, "USB MIDI keyboard disconnected");
            staff_display_test_midi_disconnected();
            break;
        case USB_MIDI_INPUT_NOTE_ON:
            staff_display_test_midi_note_on(event.channel, event.note,
                                            event.velocity);
            break;
        case USB_MIDI_INPUT_NOTE_OFF:
            /* The test page records note attacks only. Note Off is retained in
             * the queue API for the later duration/recognition pipeline. */
            break;
        default:
            break;
        }
    }
}

static void usb_midi_init(void)
{
    esp_err_t err = usb_midi_input_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB MIDI input unavailable: %s", esp_err_to_name(err));
        return;
    }

    QueueHandle_t queue = usb_midi_input_event_queue();
    if (!queue || xTaskCreate(usb_midi_staff_bridge_task, "midi_staff",
                              4096, queue, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create USB MIDI staff bridge task");
        return;
    }
    ESP_LOGI(TAG, "USB-A MIDI receive path ready");
}

void app_main(void)
{
    ESP_LOGI(TAG, "SmartScore on WT99P4C5-S1 starting...");

    /* ESP32-P4 owns display, touch, UI and networking only. */
    display_init();
    ESP_ERROR_CHECK(app_state_init());

    esp_err_t sd_err = bsp_sdcard_mount();
    if (sd_err == ESP_OK) {
        bsp_display_lock(portMAX_DELAY);
        esp_err_t font_err = app_font_init();
        bsp_display_unlock();
        if (font_err != ESP_OK)
            ESP_LOGW(TAG, "SD mounted but external Chinese font unavailable");
    } else {
        ESP_LOGW(TAG, "SD card unavailable (%s); using built-in fallback fonts",
                 esp_err_to_name(sd_err));
    }

    bsp_display_lock(portMAX_DELAY);
    setup_ui(&guider_ui);
    apply_main_menu_fonts();
    events_init(&guider_ui);
    score_ui_flow_set_start_callback(start_selected_score, NULL);
    ESP_ERROR_CHECK(creator_mode_init(&guider_ui));
    if (guider_ui.screen_btn_1) {
        lv_obj_add_event_cb(guider_ui.screen_btn_1,
                            on_animation_test_button_click,
                            LV_EVENT_CLICKED, NULL);
    }
    bsp_display_unlock();

    /* MIDI and score updates are delivered by the network manager. */
    music_display_start();

    /* USB-A electronic keyboard events use a separate queue consumer task. */
    usb_midi_init();

    /* ESP32-C5 runs matching ESP-Hosted 2.12.6 slave firmware. */
    ESP_ERROR_CHECK(wifi_manager_init());

    if (hosted_sdio_config_is_valid()) {
        s_hosted_events = xEventGroupCreate();
        assert(s_hosted_events != NULL);

        ESP_ERROR_CHECK(esp_event_handler_register(ESP_HOSTED_EVENT,
                                                    ESP_EVENT_ANY_ID,
                                                    hosted_event_handler,
                                                    NULL));

        esp_err_t hosted_err = esp_hosted_init();
        if (hosted_err != ESP_OK) {
            ESP_LOGE(TAG, "ESP-Hosted init failed: %s; UI will remain available",
                     esp_err_to_name(hosted_err));
        } else if (xTaskCreate(network_start_task, "network_start", 4096,
                               NULL, 4, NULL) != pdPASS) {
            ESP_LOGE(TAG, "failed to create network start task");
        }
    } else {
        ESP_LOGE(TAG, "network disabled until sdkconfig SDIO pins are corrected");
    }

    ESP_LOGI(TAG, "SmartScore UI ready");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
