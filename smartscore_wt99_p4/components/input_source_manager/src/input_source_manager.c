#include "input_source_manager.h"

#include <string.h>

#include "audio_s3_adapter.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define INPUT_NVS_NAMESPACE "smartscore_in"
#define INPUT_NVS_SOURCE_KEY "source"

typedef struct {
    SemaphoreHandle_t lock;
    input_source_t selected;
    input_source_t active;
    bool locked;
} input_manager_t;

static const char *TAG = "INPUT_SOURCE";
static input_manager_t s_manager;

static bool selectable_source(input_source_t source)
{
    return source == INPUT_SOURCE_USB_MIDI ||
           source == INPUT_SOURCE_AUDIO_S3;
}

static esp_err_t save_source(input_source_t source)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(INPUT_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, INPUT_NVS_SOURCE_KEY,
                      input_source_name(source));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static input_source_t load_source(void)
{
    char value[16] = {0};
    size_t length = sizeof(value);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(INPUT_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, INPUT_NVS_SOURCE_KEY, value, &length);
        nvs_close(handle);
    }
    input_source_t source = INPUT_SOURCE_USB_MIDI;
    if (err == ESP_OK && input_source_from_name(value, &source) &&
        selectable_source(source)) {
        return source;
    }
    return INPUT_SOURCE_USB_MIDI;
}

esp_err_t input_source_manager_init(void)
{
    if (s_manager.lock != NULL) {
        return ESP_OK;
    }
    memset(&s_manager, 0, sizeof(s_manager));
    s_manager.lock = xSemaphoreCreateMutex();
    if (s_manager.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = audio_s3_adapter_init();
    if (err != ESP_OK) {
        vSemaphoreDelete(s_manager.lock);
        memset(&s_manager, 0, sizeof(s_manager));
        return err;
    }
    s_manager.selected = load_source();
    s_manager.active = s_manager.selected;
    ESP_LOGI(TAG, "selected input restored: %s",
             input_source_name(s_manager.selected));
    return ESP_OK;
}

esp_err_t input_source_manager_select(input_source_t source)
{
    if (s_manager.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!selectable_source(source)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    if (s_manager.locked) {
        xSemaphoreGive(s_manager.lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = save_source(source);
    if (err == ESP_OK) {
        s_manager.selected = source;
        s_manager.active = source;
        ESP_LOGI(TAG, "selected input changed: %s",
                 input_source_name(source));
    }
    xSemaphoreGive(s_manager.lock);
    return err;
}

esp_err_t input_source_manager_lock(input_source_t *out_source)
{
    if (s_manager.lock == NULL || out_source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    if (s_manager.locked) {
        xSemaphoreGive(s_manager.lock);
        return ESP_ERR_INVALID_STATE;
    }

    input_source_t source = s_manager.selected;
    if (source == INPUT_SOURCE_USB_MIDI) {
        usb_midi_status_t midi;
        usb_midi_get_status(&midi);
        if (!midi.connected) {
            xSemaphoreGive(s_manager.lock);
            return ESP_ERR_NOT_FOUND;
        }
    } else if (source == INPUT_SOURCE_AUDIO_S3) {
        audio_s3_adapter_status_t audio;
        audio_s3_adapter_get_status(&audio);
        if (!audio.implemented) {
            xSemaphoreGive(s_manager.lock);
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (!audio.connected) {
            xSemaphoreGive(s_manager.lock);
            return ESP_ERR_NOT_FOUND;
        }
        esp_err_t err = audio_s3_adapter_start_session();
        if (err != ESP_OK) {
            (void)audio_s3_adapter_stop_session();
            xSemaphoreGive(s_manager.lock);
            return err;
        }
    } else {
        xSemaphoreGive(s_manager.lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_manager.locked = true;
    s_manager.active = source;
    *out_source = source;
    xSemaphoreGive(s_manager.lock);
    return ESP_OK;
}

void input_source_manager_unlock(void)
{
    if (s_manager.lock == NULL) {
        return;
    }
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    const bool stop_audio =
        s_manager.locked && s_manager.active == INPUT_SOURCE_AUDIO_S3;
    s_manager.locked = false;
    s_manager.active = s_manager.selected;
    xSemaphoreGive(s_manager.lock);
    if (stop_audio) {
        esp_err_t err = audio_s3_adapter_stop_session();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "unable to stop audio S3 session: %s",
                     esp_err_to_name(err));
        }
    }
}

void input_source_manager_get_status(input_source_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }
    memset(out_status, 0, sizeof(*out_status));
    if (s_manager.lock == NULL) {
        return;
    }
    xSemaphoreTake(s_manager.lock, portMAX_DELAY);
    out_status->selected_input = s_manager.selected;
    out_status->active_input = s_manager.active;
    out_status->input_locked = s_manager.locked;
    xSemaphoreGive(s_manager.lock);

    usb_midi_status_t midi;
    usb_midi_get_status(&midi);
    out_status->usb_midi_connected = midi.connected;
    out_status->usb_midi_vid = midi.vid;
    out_status->usb_midi_pid = midi.pid;
    strlcpy(out_status->usb_midi_product, midi.product,
            sizeof(out_status->usb_midi_product));

    audio_s3_adapter_status_t audio;
    audio_s3_adapter_get_status(&audio);
    out_status->audio_s3_connected = audio.connected;
    out_status->audio_s3_implemented = audio.implemented;
}
