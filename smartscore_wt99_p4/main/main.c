#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "ble_provisioning.h"
#include "board_audio.h"
#include "board_sdcard.h"
#include "board_wt99.h"
#include "cJSON.h"
#include "device_api.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_source_manager.h"
#include "network_provisioning.h"
#include "nvs_flash.h"
#include "performance_recorder.h"
#include "sdkconfig.h"
#include "score_data.h"
#include "scoring_service.h"
#include "screen_adapter.h"
#include "s3_bus.h"
#include "s3_devices.h"
#include "s3_voice_link_protocol.h"
#include "speaker_service.h"
#include "wake_prompt.h"
#include "usb_midi.h"
#include "web_provisioning.h"
#include "wifi_remote.h"

static const char *TAG = "NET";
static const char *VOICE_TAG = "VOICE_V2";
static unsigned s_web_success_grace_seconds;
static bool s_voice_s3_ready;
static bool s_voice_credentials_sent;

typedef struct {
    bool active;
    bool restore_metronome;
    bool restore_file;
    uint16_t metronome_bpm;
    uint8_t metronome_beats;
    uint8_t metronome_unit;
    char file_name[SPEAKER_FILE_NAME_MAX];
} voice_audio_focus_t;

static voice_audio_focus_t s_voice_audio_focus;
static portMUX_TYPE s_voice_audio_focus_lock = portMUX_INITIALIZER_UNLOCKED;

static void release_voice_audio_focus(void);

static void *json_psram_malloc(size_t size)
{
    void *memory = heap_caps_malloc(size,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == NULL) {
        memory = heap_caps_malloc(size,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return memory;
}

static void json_heap_free(void *memory)
{
    heap_caps_free(memory);
}

static void configure_json_allocator(void)
{
    cJSON_Hooks hooks = {
        .malloc_fn = json_psram_malloc,
        .free_fn = json_heap_free,
    };
    cJSON_InitHooks(&hooks);
    ESP_LOGI(TAG, "cJSON allocations prefer PSRAM");
}

static void acquire_voice_audio_focus(void)
{
    if (!speaker_service_is_ready()) return;
    portENTER_CRITICAL(&s_voice_audio_focus_lock);
    bool already_active = s_voice_audio_focus.active;
    portEXIT_CRITICAL(&s_voice_audio_focus_lock);
    if (already_active) return;
    speaker_status_t status;
    speaker_service_get_status(&status);
    portENTER_CRITICAL(&s_voice_audio_focus_lock);
    if (s_voice_audio_focus.active) {
        portEXIT_CRITICAL(&s_voice_audio_focus_lock);
        return;
    }
    memset(&s_voice_audio_focus, 0, sizeof(s_voice_audio_focus));
    s_voice_audio_focus.active = true;

    if (status.metronome.state == SPEAKER_METRONOME_RUNNING) {
        s_voice_audio_focus.restore_metronome = true;
        s_voice_audio_focus.metronome_bpm = status.metronome.bpm;
        s_voice_audio_focus.metronome_beats =
            status.metronome.beats_per_measure;
        s_voice_audio_focus.metronome_unit = status.metronome.beat_unit;
    }
    if (status.state == SPEAKER_STATE_FILE) {
        s_voice_audio_focus.restore_file = true;
        strlcpy(s_voice_audio_focus.file_name, status.file_name,
                sizeof(s_voice_audio_focus.file_name));
    }
    portEXIT_CRITICAL(&s_voice_audio_focus_lock);

    if (status.metronome.state == SPEAKER_METRONOME_RUNNING) {
        esp_err_t err = speaker_service_metronome_pause();
        ESP_LOGI(TAG, "voice audio focus: metronome pause %s",
                 esp_err_to_name(err));
    }
    if (status.state == SPEAKER_STATE_FILE) {
        esp_err_t err = speaker_service_pause_file();
        ESP_LOGI(TAG, "voice audio focus: file pause %s",
                 esp_err_to_name(err));
    }
    if (status.state == SPEAKER_STATE_TONE) {
        esp_err_t err = speaker_service_stop();
        ESP_LOGI(TAG, "voice audio focus: transient tone stop %s",
                 esp_err_to_name(err));
    }
    ESP_LOGI(VOICE_TAG, "speaker background paused");
}

static void begin_voice_audio_focus(void)
{
    portENTER_CRITICAL(&s_voice_audio_focus_lock);
    bool stale_focus = s_voice_audio_focus.active;
    portEXIT_CRITICAL(&s_voice_audio_focus_lock);
    if (stale_focus) {
        ESP_LOGW(TAG, "releasing stale voice audio focus before new WAKE");
        release_voice_audio_focus();
    }
    acquire_voice_audio_focus();
}

static void release_voice_audio_focus(void)
{
    if (!speaker_service_is_ready()) return;
    portENTER_CRITICAL(&s_voice_audio_focus_lock);
    if (!s_voice_audio_focus.active) {
        portEXIT_CRITICAL(&s_voice_audio_focus_lock);
        return;
    }
    voice_audio_focus_t saved = s_voice_audio_focus;
    memset(&s_voice_audio_focus, 0, sizeof(s_voice_audio_focus));
    portEXIT_CRITICAL(&s_voice_audio_focus_lock);
    speaker_status_t status;
    speaker_service_get_status(&status);

    if (saved.restore_metronome &&
        status.metronome.state == SPEAKER_METRONOME_PAUSED) {
        esp_err_t err = speaker_service_metronome_start(
            saved.metronome_bpm, saved.metronome_beats,
            saved.metronome_unit);
        ESP_LOGI(TAG, "voice audio focus: metronome restore %s",
                 esp_err_to_name(err));
    }
    if (saved.restore_file && status.state == SPEAKER_STATE_FILE_PAUSED &&
        strcmp(status.file_name, saved.file_name) == 0) {
        esp_err_t err = speaker_service_pause_file();
        ESP_LOGI(TAG, "voice audio focus: file resume %s",
                 esp_err_to_name(err));
    }
    ESP_LOGI(VOICE_TAG, "speaker background restored");
}

/* Live colouring uses a display-only latency estimate. Final scoring uses
 * S3 sender timestamps, so UART polling jitter cannot distort note spacing. */
#define AUDIO_S3_PIPELINE_LATENCY_US 160000ULL

static void handle_usb_midi_event(const usb_midi_event_t *event, void *context)
{
    (void)context;
    scoring_service_handle_usb_event(event);
    screen_adapter_handle_usb_midi_event(event);
}

static void handle_music_s3_event(const s3_music_event_t *event, void *context)
{
    (void)context;
    if (event == NULL) {
        return;
    }
    screen_adapter_handle_audio_s3_event(event);
    /* Native poly frames are diagnostic snapshots. Audio S3 separately emits
     * its selected melody note_on/off stream for the unchanged scorer. */
    if (event->type == S3_MUSIC_EVENT_POLY ||
        event->type == S3_MUSIC_EVENT_DIAGNOSTIC) {
        return;
    }
    const uint64_t received_us = (uint64_t)esp_timer_get_time();
    const usb_midi_event_t midi_event = {
        .timestamp_us =
            received_us > AUDIO_S3_PIPELINE_LATENCY_US
                ? received_us - AUDIO_S3_PIPELINE_LATENCY_US
                : received_us,
        .type = event->type == S3_MUSIC_EVENT_NOTE_ON
                    ? USB_MIDI_EVENT_NOTE_ON
                    : USB_MIDI_EVENT_NOTE_OFF,
        .cable = 0,
        .channel = 0,
        .midi = event->midi,
        .velocity = event->type == S3_MUSIC_EVENT_NOTE_ON
                        ? event->velocity
                        : 0,
    };
    scoring_service_handle_audio_s3_event(
        event->type == S3_MUSIC_EVENT_NOTE_ON,
        event->sid, event->sender_ts_ms, event->midi, event->velocity,
        event->has_confidence ? event->confidence : 0.75f,
        event->has_frequency ? event->frequency_hz : 0.0f);
    screen_adapter_handle_usb_midi_event(&midi_event);
}

static void handle_voice_command_result(uint8_t command_id,
                                        bool handled,
                                        void *context)
{
    (void)context;
    esp_err_t err = s3_voice_node_send_ack(command_id, handled);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "voice S3 ACK failed: %s", esp_err_to_name(err));
    }
    release_voice_audio_focus();
}

static void handle_voice_s3_event(const s3_voice_event_t *event,
                                  void *context)
{
    (void)context;
    if (event == NULL) return;

    if (event->type == S3_VOICE_EVENT_AI_STATE) {
        switch (event->ai_state) {
        case S3_VOICE_AI_STATE_LISTENING:
            acquire_voice_audio_focus();
            ESP_LOGI(VOICE_TAG, "state=LISTENING owner=voice-S3");
            break;
        case S3_VOICE_AI_STATE_THINKING:
            ESP_LOGI(VOICE_TAG, "state=THINKING owner=voice-S3");
            break;
        case S3_VOICE_AI_STATE_SPEAKING:
            ESP_LOGI(VOICE_TAG, "state=SPEAKING owner=voice-S3");
            break;
        case S3_VOICE_AI_STATE_ERROR:
            ESP_LOGE(VOICE_TAG, "state=ERROR owner=voice-S3");
            break;
        default:
            ESP_LOGI(VOICE_TAG, "state=IDLE owner=voice-S3");
            release_voice_audio_focus();
            break;
        }
        return;
    }

    if (event->type == S3_VOICE_EVENT_AI_AUDIO_START) {
        acquire_voice_audio_focus();
        esp_err_t err = speaker_service_stream_start(event->sample_rate_hz);
        if (err != ESP_OK) {
            ESP_LOGE(VOICE_TAG, "speaker stream start failed: %s",
                     esp_err_to_name(err));
            (void)s3_voice_node_send_ai_stop("SPEAKER_START_ERROR");
        } else {
            ESP_LOGI(VOICE_TAG,
                     "AI PCM stream started rate=%" PRIu32 "Hz format=s16le/mono prebuffer=48KiB",
                     event->sample_rate_hz);
        }
        return;
    }
    if (event->type == S3_VOICE_EVENT_AI_AUDIO_DONE) {
        ESP_LOGI(VOICE_TAG, "AI PCM response done; speaker drain pending");
        return;
    }
    if (event->type == S3_VOICE_EVENT_AI_ERROR) {
        ESP_LOGE(VOICE_TAG, "voice-S3 Qwen error");
        (void)speaker_service_stream_abort();
        release_voice_audio_focus();
        return;
    }
    if (event->type == S3_VOICE_EVENT_AI_TEXT) {
        ESP_LOGI(VOICE_TAG, "assistant: %s",
                 event->text != NULL ? event->text : "");
        return;
    }

    screen_voice_event_type_t type;
    switch (event->type) {
    case S3_VOICE_EVENT_WAKE:
        begin_voice_audio_focus();
        {
            esp_err_t prompt_error = wake_prompt_play();
            if (prompt_error != ESP_OK) {
                ESP_LOGW(VOICE_TAG, "wake prompt queue failed: %s",
                         esp_err_to_name(prompt_error));
            }
        }
        ESP_LOGI(VOICE_TAG,
                 "wake acknowledged by audio; listening popup disabled");
        return;
    case S3_VOICE_EVENT_TIMEOUT:
        release_voice_audio_focus();
        type = SCREEN_VOICE_EVENT_TIMEOUT;
        break;
    case S3_VOICE_EVENT_COMMAND:
        type = SCREEN_VOICE_EVENT_COMMAND;
        break;
    default:
        return;
    }

    esp_err_t err = screen_adapter_handle_voice_event(
        type, event->command_id,
        event->type == S3_VOICE_EVENT_COMMAND
            ? handle_voice_command_result : NULL,
        NULL);
    if (err != ESP_OK && event->type == S3_VOICE_EVENT_COMMAND) {
        (void)s3_voice_node_send_ack(event->command_id, false);
        release_voice_audio_focus();
    }
}

static void handle_camera_command_result(uint8_t command_id,
                                         bool handled,
                                         void *context)
{
    (void)context;
    esp_err_t err = s3_camera_node_send_ack(command_id, handled);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "camera S3 ACK failed: %s", esp_err_to_name(err));
    }
}

static void handle_camera_s3_event(const s3_camera_event_t *event,
                                   void *context)
{
    (void)context;
    if (event == NULL) return;

    esp_err_t err = screen_adapter_handle_voice_event(
        SCREEN_VOICE_EVENT_COMMAND, event->command_id,
        handle_camera_command_result, NULL);
    if (err != ESP_OK) {
        (void)s3_camera_node_send_ack(event->command_id, false);
    }
}

static esp_err_t initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition requires reinitialization");
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    return err;
}

static void blocked_status_loop(const char *stage, esp_err_t error)
{
    while (true) {
        ESP_LOGE(TAG, "network startup blocked at %s: %s; %s",
                 stage,
                 esp_err_to_name(error),
                 board_wt99_network_block_reason());
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void manage_network_services(network_state_t state)
{
    if (state == NETWORK_STATE_WIFI_CONNECTED) {
        if (s_voice_s3_ready && !s_voice_credentials_sent) {
            char ssid[33] = {0};
            char password[65] = {0};
            esp_err_t credential_error =
                network_provisioning_get_saved_credentials(
                    ssid, sizeof(ssid), password, sizeof(password));
            if (credential_error == ESP_OK) {
                credential_error = s3_voice_node_send_wifi_credentials(
                    ssid, password);
                if (credential_error == ESP_OK) {
                    s_voice_credentials_sent = true;
                    ESP_LOGI(TAG,
                             "Wi-Fi credentials forwarded to voice S3; Qwen realtime leaves P4/C5 path");
                }
            }
            memset(password, 0, sizeof(password));
        }
        if (web_provisioning_is_running()) {
            /* Let the setup page poll and display the acquired IP first. */
            if (s_web_success_grace_seconds++ < 5U) {
                return;
            }
            esp_err_t err = web_provisioning_stop();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "web fallback stop failed: %s", esp_err_to_name(err));
                return;
            }
            err = wifi_remote_disable_ap();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "fallback AP disable failed: %s", esp_err_to_name(err));
            }
        }
        if (!device_api_is_running()) {
            esp_err_t err = device_api_start();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "device API start failed: %s", esp_err_to_name(err));
            }
        }
        return;
    }

    s_voice_credentials_sent = false;
    s_web_success_grace_seconds = 0;

    if (device_api_is_running()) {
        device_api_stop();
    }
    if (state == NETWORK_STATE_WIFI_CONNECTING && web_provisioning_is_running()) {
        web_provisioning_stop();
    }

#if CONFIG_SMARTSCORE_WEB_PROVISIONING_FALLBACK
    if ((state == NETWORK_STATE_WAITING_CREDENTIALS ||
         state == NETWORK_STATE_WIFI_FAILED) &&
        !web_provisioning_is_running()) {
        esp_err_t err = web_provisioning_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "web fallback start failed: %s", esp_err_to_name(err));
        }
    }
#endif
}

void app_main(void)
{
    /* Install the global hooks before any service can parse or build JSON. */
    configure_json_allocator();
    ESP_LOGI(TAG, "SmartScore WT99 stage-one network starting");

    esp_err_t err = initialize_nvs();
    if (err != ESP_OK) {
        blocked_status_loop("NVS initialization", err);
    }

    err = board_sdcard_mount();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MicroSD unavailable: %s; continuing without external storage",
                 esp_err_to_name(err));
    }

#if CONFIG_SMARTSCORE_SPEAKER_ENABLED
    err = speaker_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "speaker service unavailable: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "speaker service initialized; amplifier remains off");
        esp_err_t prompt_error = wake_prompt_init();
        if (prompt_error != ESP_OK) {
            ESP_LOGW(TAG, "wake prompt unavailable: %s",
                     esp_err_to_name(prompt_error));
        }
#if CONFIG_SMARTSCORE_SPEAKER_STARTUP_TEST
        err = speaker_service_play_tone(440.0f, 1000, 0.12f);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "speaker startup tone queue failed: %s",
                     esp_err_to_name(err));
        }
#endif
    }
#else
    err = board_audio_force_disabled();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to force onboard amplifier off: %s",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG,
                 "speaker-disabled build: ES8311/I2S not initialized, amplifier held off");
    }
    err = speaker_service_init_control_only();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio control state unavailable: %s",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG,
                 "audio controls ready in state-only mode for screen/API synchronization");
    }
#endif

    ESP_LOGI(TAG,
             "P4 legacy Qwen WebSocket runtime disabled; P4 is AI PCM playback endpoint only");

    err = performance_recorder_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "performance recorder unavailable: %s",
                 esp_err_to_name(err));
    }

    err = score_data_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "score data unavailable: %s", esp_err_to_name(err));
    }

    err = input_source_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "input source manager unavailable: %s",
                 esp_err_to_name(err));
    }

    err = scoring_service_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scoring service unavailable: %s",
                 esp_err_to_name(err));
    } else {
        usb_midi_set_event_handler(handle_usb_midi_event, NULL);
    }

    err = usb_midi_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB MIDI unavailable: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "USB MIDI host initialized");
    }

    err = s3_bus_init(handle_music_s3_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "music S3 UART unavailable: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "music S3 bidirectional UART initialized");
    }

    err = screen_adapter_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "screen unavailable: %s; other services continue",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "screen initialization scheduled");
    }

    err = s3_voice_node_init(handle_voice_s3_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "voice S3 UART unavailable: %s", esp_err_to_name(err));
    } else {
        s_voice_s3_ready = true;
        ESP_LOGI(TAG, "voice S3 bidirectional UART initialized");
        err = s3_voice_node_send_ai_stop("P4_READY");
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "voice S3 startup synchronization failed: %s",
                     esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "voice S3 startup state synchronized");
        }
    }

    err = s3_camera_node_init(handle_camera_s3_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera S3 UART unavailable: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "camera S3 bidirectional UART initialized");
    }

    ESP_LOGI(TAG, "preparing C5 transport");
    err = board_wt99_network_power_enable();
    if (err != ESP_OK) {
        blocked_status_loop("C5 power control", err);
    }
    err = board_wt99_c5_reset();
    if (err != ESP_OK) {
        blocked_status_loop("C5 reset", err);
    }
    err = board_wt99_c5_transport_prepare();
    if (err != ESP_OK) {
        blocked_status_loop("C5 transport preparation", err);
    }
    ESP_LOGI("BOARD", "C5 transport prepared");

    err = wifi_remote_platform_init();
    if (err != ESP_OK) {
        blocked_status_loop("network platform initialization", err);
    }
    err = network_provisioning_init();
    if (err != ESP_OK) {
        blocked_status_loop("provisioning initialization", err);
    }
    network_provisioning_report_hosted_starting();

    err = wifi_remote_hosted_start();
    if (err != ESP_OK) {
        blocked_status_loop("ESP-Hosted initialization", err);
    }
    err = wifi_remote_init();
    if (err != ESP_OK) {
        blocked_status_loop("Wi-Fi Remote initialization", err);
    }
    network_provisioning_report_hosted_ready();

    err = ble_provisioning_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE provisioning unavailable: %s", esp_err_to_name(err));
#if !CONFIG_SMARTSCORE_WEB_PROVISIONING_FALLBACK
        blocked_status_loop("C5 Hosted Bluetooth capability", err);
#endif
    }

    err = network_provisioning_start();
    if (err != ESP_OK) {
        blocked_status_loop("provisioning task start", err);
    }

    wifi_remote_capabilities_t capabilities = wifi_remote_get_capabilities();
    ESP_LOGI(TAG, "remote capabilities: WIFI=%s BLE=%s",
             capabilities.wifi_available ? "YES" : "NO",
             capabilities.ble_available ? "YES" : "NO");

    while (true) {
        network_status_t status = network_provisioning_get_status();
        manage_network_services(status.state);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
