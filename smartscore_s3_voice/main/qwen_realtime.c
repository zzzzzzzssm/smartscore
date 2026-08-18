#include "qwen_realtime.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "qwen_protocol.h"
#include "sdkconfig.h"
#include "voice_link_protocol.h"
#include "voice_uart_link.h"

#ifndef CONFIG_SMARTSCORE_QWEN_REALTIME_ENABLED
#define CONFIG_SMARTSCORE_QWEN_REALTIME_ENABLED 1
#endif
#ifndef CONFIG_SMARTSCORE_QWEN_REALTIME_MODEL
#define CONFIG_SMARTSCORE_QWEN_REALTIME_MODEL "qwen-audio-3.0-realtime-plus"
#endif
#ifndef CONFIG_SMARTSCORE_QWEN_REALTIME_ENDPOINT
#define CONFIG_SMARTSCORE_QWEN_REALTIME_ENDPOINT \
    "wss://dashscope.aliyuncs.com/api-ws/v1/realtime"
#endif
#ifndef CONFIG_SMARTSCORE_QWEN_REALTIME_VOICE
#define CONFIG_SMARTSCORE_QWEN_REALTIME_VOICE "longanqian"
#endif
#ifndef CONFIG_SMARTSCORE_QWEN_REALTIME_INSTRUCTIONS
#define CONFIG_SMARTSCORE_QWEN_REALTIME_INSTRUCTIONS \
    "You are the SmartScore piano voice assistant. Answer naturally and concisely in Chinese."
#endif
#ifndef QWEN_S3_API_KEY
#define QWEN_S3_API_KEY ""
#endif

#define QWEN_INPUT_RATE_HZ 16000U
#define QWEN_OUTPUT_RATE_HZ 24000U
#define QWEN_INPUT_BLOCK_SAMPLES 512U
#define QWEN_INPUT_BLOCK_COUNT 112U
#define QWEN_INPUT_SEND_BLOCKS 4U
#define QWEN_INPUT_JSON_BYTES 8192U
#define QWEN_COMMAND_QUEUE_LENGTH 20U
#define QWEN_WS_SLOT_COUNT 4U
#define QWEN_WS_SLOT_BYTES (192U * 1024U)
#define QWEN_WS_TASK_STACK_BYTES 6144U
#define QWEN_WS_BUFFER_BYTES 4096U
#define QWEN_OWNER_STACK_BYTES (16U * 1024U)
#define QWEN_UART_STACK_BYTES (6U * 1024U)
#define QWEN_OUTPUT_RING_BYTES (128U * 1024U)
#define QWEN_OUTPUT_HIGH_WATER (96U * 1024U)
#define QWEN_DECODE_BYTES 6144U
#define QWEN_SEND_TIMEOUT_MS 1500U
#define QWEN_STALL_TIMEOUT_US 12000000ULL
#define QWEN_DRAIN_TIMEOUT_US 8000000ULL
#define QWEN_SUMMARY_INTERVAL_US 1000000ULL
#define QWEN_AUDIO_GAP_WARN_US 800000ULL
#define QWEN_CALLBACK_WARN_US 20000ULL

typedef enum {
    CMD_ARM = 0,
    CMD_BEGIN,
    CMD_SPEECH_END,
    CMD_LOCAL_CANCEL,
    CMD_STOP,
    CMD_P4_DRAINED,
    CMD_WIFI_CREDENTIALS,
    CMD_WIFI_UP,
    CMD_WIFI_DOWN,
    CMD_WS_CONNECTED,
    CMD_WS_DISCONNECTED,
    CMD_WS_ERROR,
} qwen_command_type_t;

typedef struct {
    qwen_command_type_t type;
    char ssid[33];
    char password[65];
    esp_err_t error;
} qwen_command_t;

typedef struct {
    uint16_t sample_count;
    int16_t pcm[QWEN_INPUT_BLOCK_SAMPLES];
} qwen_input_block_t;

typedef struct {
    uint8_t *data;
    size_t length;
    bool used;
} qwen_ws_slot_t;

typedef struct {
    int slot;
    size_t frame_expected;
    size_t frame_received;
    uint8_t opcode;
    bool active;
} qwen_ws_assembler_t;

static const char *TAG = "QWEN_S3";
static bool s_initialized;
static volatile bool s_accept_pcm;
static volatile bool s_stop_requested;
static volatile bool s_uart_flow_enabled = true;
static volatile bool s_output_done_pending;
static volatile bool s_output_done_sent;
static volatile bool s_p4_drained;
static volatile qwen_voice_state_t s_state = QWEN_VOICE_IDLE;
static bool s_wifi_have_credentials;
static volatile bool s_wifi_got_ip;
static bool s_ws_connected;
static bool s_ws_starting;
static bool s_session_update_sent;
static bool s_session_ready;
static bool s_remote_cleared;
static bool s_route_confirmed;
static bool s_speech_end;
static bool s_commit_sent;
static bool s_response_created;
static bool s_audio_started;
static bool s_audio_done;
static bool s_response_done;
static uint64_t s_last_business_us;
static uint64_t s_last_audio_us;
static uint64_t s_drain_wait_started_us;
static uint64_t s_next_summary_us;
static uint64_t s_mic_pcm_up;
static uint64_t s_ai_pcm_down;
static uint64_t s_uart_pcm_sent;
static uint64_t s_max_audio_gap_us;
static uint32_t s_audio_delta_index;
static uint32_t s_uart_errors;
static uint32_t s_input_drops;
static uint32_t s_output_high_events;
static char s_last_server_event[96] = "none";

static QueueHandle_t s_command_queue;
static QueueHandle_t s_input_queue;
static QueueHandle_t s_message_queue;
static qwen_input_block_t *s_input_blocks;
static uint16_t s_input_free[QWEN_INPUT_BLOCK_COUNT];
static size_t s_input_free_count;
static portMUX_TYPE s_input_lock = portMUX_INITIALIZER_UNLOCKED;
static uint16_t s_pending_indices[QWEN_INPUT_SEND_BLOCKS];
static size_t s_pending_count;
static size_t s_pending_bytes;
static uint8_t *s_pending_pcm;

static qwen_ws_slot_t s_ws_slots[QWEN_WS_SLOT_COUNT];
static qwen_ws_assembler_t s_assembler = {.slot = -1};
static portMUX_TYPE s_ws_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_ws_slots_in_use;
static uint32_t s_ws_slot_exhaustions;
static uint64_t s_ws_rx_events;
static uint64_t s_ws_rx_bytes;
static uint64_t s_ws_callback_max_us;

static uint8_t *s_output_ring;
static size_t s_output_read;
static size_t s_output_write;
static size_t s_output_count;
static size_t s_output_max;
static SemaphoreHandle_t s_output_lock;

static char *s_tx_json;
static uint8_t *s_decode_pcm;
static esp_websocket_client_handle_t s_ws_client;
static char s_ws_uri[320];
static char s_ws_headers[384];
static TaskHandle_t s_owner_task;
static TaskHandle_t s_uart_task;
static StackType_t *s_owner_stack;
static StaticTask_t s_owner_tcb;

static void qwen_websocket_event_handler(void *handler_arg,
                                         esp_event_base_t base,
                                         int32_t event_id,
                                         void *event_data);

static void wake_uart_task(void)
{
    if (s_uart_task != NULL) xTaskNotifyGive(s_uart_task);
}

static const char *state_name(qwen_voice_state_t state)
{
    switch (state) {
    case QWEN_VOICE_IDLE: return "IDLE";
    case QWEN_VOICE_CANDIDATE: return "CANDIDATE";
    case QWEN_VOICE_LISTENING: return "LISTENING";
    case QWEN_VOICE_THINKING: return "THINKING";
    case QWEN_VOICE_SPEAKING: return "SPEAKING";
    case QWEN_VOICE_DRAINING: return "DRAINING";
    case QWEN_VOICE_ERROR: return "ERROR";
    default: return "?";
    }
}

static void set_state(qwen_voice_state_t next, const char *reason)
{
    qwen_voice_state_t previous = s_state;
    if (previous == next) return;
    s_state = next;
    ESP_LOGI(TAG, "state %s -> %s reason=%s", state_name(previous),
             state_name(next), reason != NULL ? reason : "-");
}

static uint8_t public_state(qwen_voice_state_t state)
{
    switch (state) {
    case QWEN_VOICE_CANDIDATE:
    case QWEN_VOICE_LISTENING: return VOICE_AI_STATE_LISTENING;
    case QWEN_VOICE_THINKING: return VOICE_AI_STATE_THINKING;
    case QWEN_VOICE_SPEAKING:
    case QWEN_VOICE_DRAINING: return VOICE_AI_STATE_SPEAKING;
    case QWEN_VOICE_ERROR: return VOICE_AI_STATE_ERROR;
    default: return VOICE_AI_STATE_IDLE;
    }
}

static void send_p4_state(qwen_voice_state_t state)
{
    const uint8_t value = public_state(state);
    if (voice_uart_link_send_message(VOICE_MSG_AI_STATE, &value,
                                     sizeof(value)) != ESP_OK) {
        ++s_uart_errors;
    }
}

static bool queue_command(const qwen_command_t *command)
{
    if (!s_initialized || command == NULL || s_command_queue == NULL)
        return false;
    if (xQueueSend(s_command_queue, command, 0) != pdTRUE) return false;
    if (s_owner_task != NULL) xTaskNotifyGive(s_owner_task);
    return true;
}

static bool allocate_input(uint16_t *index)
{
    bool ok = false;
    portENTER_CRITICAL(&s_input_lock);
    if (s_input_free_count > 0U) {
        *index = s_input_free[--s_input_free_count];
        ok = true;
    }
    portEXIT_CRITICAL(&s_input_lock);
    return ok;
}

static void release_input(uint16_t index)
{
    portENTER_CRITICAL(&s_input_lock);
    if (s_input_free_count < QWEN_INPUT_BLOCK_COUNT)
        s_input_free[s_input_free_count++] = index;
    portEXIT_CRITICAL(&s_input_lock);
}

static void release_pending_input(void)
{
    for (size_t i = 0U; i < s_pending_count; ++i)
        release_input(s_pending_indices[i]);
    s_pending_count = 0U;
    s_pending_bytes = 0U;
}

static void drain_input(void)
{
    release_pending_input();
    uint16_t index;
    while (xQueueReceive(s_input_queue, &index, 0) == pdTRUE)
        release_input(index);
}

static size_t output_buffered(void)
{
    size_t count = 0U;
    if (xSemaphoreTake(s_output_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
        count = s_output_count;
        xSemaphoreGive(s_output_lock);
    }
    return count;
}

static void output_clear(void)
{
    if (xSemaphoreTake(s_output_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_output_read = 0U;
        s_output_write = 0U;
        s_output_count = 0U;
        s_output_max = 0U;
        xSemaphoreGive(s_output_lock);
    }
    s_output_done_pending = false;
    s_output_done_sent = false;
}

static esp_err_t output_write_all(const uint8_t *data, size_t length)
{
    size_t offset = 0U;
    while (offset < length && !s_stop_requested) {
        size_t copied = 0U;
        if (xSemaphoreTake(s_output_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
            size_t free_bytes = QWEN_OUTPUT_RING_BYTES - s_output_count;
            copied = length - offset;
            if (copied > free_bytes) copied = free_bytes;
            if (copied > QWEN_OUTPUT_RING_BYTES - s_output_write)
                copied = QWEN_OUTPUT_RING_BYTES - s_output_write;
            if (copied > 0U) {
                memcpy(s_output_ring + s_output_write, data + offset, copied);
                s_output_write = (s_output_write + copied) %
                                 QWEN_OUTPUT_RING_BYTES;
                s_output_count += copied;
                if (s_output_count > s_output_max) s_output_max = s_output_count;
                if (s_output_count >= QWEN_OUTPUT_HIGH_WATER)
                    ++s_output_high_events;
            }
            xSemaphoreGive(s_output_lock);
        }
        if (copied == 0U) {
            vTaskDelay(pdMS_TO_TICKS(2));
        } else {
            offset += copied;
            wake_uart_task();
        }
    }
    return offset == length ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static size_t output_read(uint8_t *data, size_t capacity)
{
    size_t copied = 0U;
    if (xSemaphoreTake(s_output_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        copied = s_output_count < capacity ? s_output_count : capacity;
        if (copied > QWEN_OUTPUT_RING_BYTES - s_output_read)
            copied = QWEN_OUTPUT_RING_BYTES - s_output_read;
        if (copied > 0U) {
            memcpy(data, s_output_ring + s_output_read, copied);
            s_output_read = (s_output_read + copied) % QWEN_OUTPUT_RING_BYTES;
            s_output_count -= copied;
        }
        xSemaphoreGive(s_output_lock);
    }
    return copied;
}

static int allocate_ws_slot(void)
{
    int result = -1;
    portENTER_CRITICAL(&s_ws_lock);
    for (int i = 0; i < (int)QWEN_WS_SLOT_COUNT; ++i) {
        if (!s_ws_slots[i].used) {
            s_ws_slots[i].used = true;
            s_ws_slots[i].length = 0U;
            ++s_ws_slots_in_use;
            result = i;
            break;
        }
    }
    if (result < 0) ++s_ws_slot_exhaustions;
    portEXIT_CRITICAL(&s_ws_lock);
    return result;
}

static void release_ws_slot(int slot)
{
    if (slot < 0 || slot >= (int)QWEN_WS_SLOT_COUNT) return;
    portENTER_CRITICAL(&s_ws_lock);
    if (s_ws_slots[slot].used) {
        s_ws_slots[slot].used = false;
        s_ws_slots[slot].length = 0U;
        if (s_ws_slots_in_use > 0U) --s_ws_slots_in_use;
    }
    portEXIT_CRITICAL(&s_ws_lock);
}

static void reset_assembler(void)
{
    if (s_assembler.active) release_ws_slot(s_assembler.slot);
    memset(&s_assembler, 0, sizeof(s_assembler));
    s_assembler.slot = -1;
}

static int websocket_send(const char *json, size_t length)
{
    if (!s_ws_connected || s_ws_client == NULL || json == NULL ||
        length == 0U || length > INT_MAX) return -1;
    return esp_websocket_client_send_text(s_ws_client, json, (int)length,
                                          pdMS_TO_TICKS(QWEN_SEND_TIMEOUT_MS));
}

static esp_err_t send_simple(const char *type)
{
    size_t length = 0U;
    esp_err_t err = qwen_protocol_build_simple_event(
        s_tx_json, QWEN_INPUT_JSON_BYTES, type, &length);
    if (err == ESP_OK && websocket_send(s_tx_json, length) != (int)length)
        err = ESP_FAIL;
    if (err == ESP_OK)
        ESP_LOGI(TAG, "CLIENT_EVENT type=%s time=%" PRIu64 "ms", type,
                 (uint64_t)esp_timer_get_time() / 1000ULL);
    return err;
}

static esp_err_t send_session_update(void)
{
    size_t length = 0U;
    esp_err_t err = qwen_protocol_build_session_update(
        s_tx_json, QWEN_INPUT_JSON_BYTES,
        CONFIG_SMARTSCORE_QWEN_REALTIME_VOICE,
        CONFIG_SMARTSCORE_QWEN_REALTIME_INSTRUCTIONS, &length);
    if (err == ESP_OK && websocket_send(s_tx_json, length) != (int)length)
        err = ESP_FAIL;
    if (err == ESP_OK) {
        s_session_update_sent = true;
        ESP_LOGI(TAG,
                 "CLIENT_EVENT type=session.update model=%s input=pcm/16000/16/1 output=pcm/24000/16/1 voice=%s",
                 CONFIG_SMARTSCORE_QWEN_REALTIME_MODEL,
                 CONFIG_SMARTSCORE_QWEN_REALTIME_VOICE);
    }
    return err;
}

static void reset_turn(void)
{
    s_accept_pcm = false;
    drain_input();
    output_clear();
    s_stop_requested = false;
    s_remote_cleared = false;
    s_route_confirmed = false;
    s_speech_end = false;
    s_commit_sent = false;
    s_response_created = false;
    s_audio_started = false;
    s_audio_done = false;
    s_response_done = false;
    s_p4_drained = false;
    s_last_business_us = 0U;
    s_last_audio_us = 0U;
    s_drain_wait_started_us = 0U;
    s_mic_pcm_up = 0U;
    s_ai_pcm_down = 0U;
    s_uart_pcm_sent = 0U;
    s_max_audio_gap_us = 0U;
    s_audio_delta_index = 0U;
    strlcpy(s_last_server_event, "none", sizeof(s_last_server_event));
}

static void finish_to_idle(const char *reason)
{
    s_accept_pcm = false;
    drain_input();
    output_clear();
    s_drain_wait_started_us = 0U;
    set_state(QWEN_VOICE_IDLE, reason);
    send_p4_state(QWEN_VOICE_IDLE);
    ESP_LOGI(TAG, "wake path re-armed");
}

static void fail_turn(const char *reason, esp_err_t error)
{
    ESP_LOGE(TAG, "turn failed reason=%s error=%s", reason,
             esp_err_to_name(error));
    if (s_response_created && s_ws_connected) (void)send_simple("response.cancel");
    if (s_session_ready && s_ws_connected) (void)send_simple("input_audio_buffer.clear");
    set_state(QWEN_VOICE_ERROR, reason);
    send_p4_state(QWEN_VOICE_ERROR);
    (void)voice_uart_link_send_message(VOICE_MSG_AI_ERROR, reason,
                                       (uint16_t)strlen(reason));
    finish_to_idle(reason);
}

static esp_err_t apply_wifi_credentials(const char *ssid,
                                        const char *password)
{
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("qwen_wifi", NVS_READWRITE, &nvs);
    if (err == ESP_OK) err = nvs_set_str(nvs, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "password", password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    if (nvs != 0) nvs_close(nvs);
    if (err != ESP_OK) return err;

    (void)esp_wifi_disconnect();
    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    memset(&config, 0, sizeof(config));
    if (err == ESP_OK) {
        s_wifi_have_credentials = true;
        err = esp_wifi_connect();
    }
    ESP_LOGI(TAG, "voice-S3 Wi-Fi credentials applied ssid=%s", ssid);
    return err;
}

static void handle_command(const qwen_command_t *command)
{
    switch (command->type) {
    case CMD_ARM:
        if (s_state != QWEN_VOICE_IDLE) finish_to_idle("new-wake-reset");
        reset_turn();
        set_state(QWEN_VOICE_CANDIDATE, "wake");
        send_p4_state(QWEN_VOICE_CANDIDATE);
        s_accept_pcm = true;
        if (s_session_ready && send_simple("input_audio_buffer.clear") == ESP_OK)
            s_remote_cleared = true;
        break;
    case CMD_BEGIN:
        if (s_state == QWEN_VOICE_CANDIDATE) {
            s_route_confirmed = true;
            set_state(QWEN_VOICE_LISTENING, "open-ai-question");
        }
        break;
    case CMD_SPEECH_END:
        if (s_state == QWEN_VOICE_CANDIDATE ||
            s_state == QWEN_VOICE_LISTENING) {
            s_accept_pcm = false;
            s_speech_end = true;
        }
        break;
    case CMD_LOCAL_CANCEL:
        s_accept_pcm = false;
        if (s_session_ready && s_ws_connected)
            (void)send_simple("input_audio_buffer.clear");
        finish_to_idle("local-command");
        break;
    case CMD_STOP:
        s_accept_pcm = false;
        if (s_response_created && s_ws_connected)
            (void)send_simple("response.cancel");
        if (s_session_ready && s_ws_connected)
            (void)send_simple("input_audio_buffer.clear");
        finish_to_idle("p4-stop");
        (void)voice_uart_link_send_message(VOICE_MSG_STOP_ACK, NULL, 0U);
        s_stop_requested = false;
        break;
    case CMD_P4_DRAINED:
        s_p4_drained = true;
        ESP_LOGI(TAG, "P4 audio drained acknowledged state=%s",
                 state_name(s_state));
        break;
    case CMD_WIFI_CREDENTIALS:
        if (apply_wifi_credentials(command->ssid, command->password) != ESP_OK)
            ESP_LOGE(TAG, "voice-S3 Wi-Fi credential apply failed");
        break;
    case CMD_WIFI_UP:
        s_wifi_got_ip = true;
        ESP_LOGI(TAG, "voice-S3 Wi-Fi got IP; Qwen transport enabled");
        break;
    case CMD_WIFI_DOWN:
        s_wifi_got_ip = false;
        s_ws_connected = false;
        s_session_ready = false;
        s_session_update_sent = false;
        if (s_state != QWEN_VOICE_IDLE)
            fail_turn("voice-s3-wifi-disconnected", ESP_ERR_INVALID_STATE);
        break;
    case CMD_WS_CONNECTED:
        s_ws_connected = true;
        s_ws_starting = false;
        s_session_update_sent = false;
        s_session_ready = false;
        ESP_LOGI(TAG, "Qwen WebSocket connected");
        break;
    case CMD_WS_DISCONNECTED:
        s_ws_connected = false;
        s_session_ready = false;
        s_session_update_sent = false;
        if (s_state != QWEN_VOICE_IDLE)
            fail_turn("qwen-websocket-disconnected", ESP_ERR_INVALID_STATE);
        break;
    case CMD_WS_ERROR:
        if (s_state != QWEN_VOICE_IDLE)
            fail_turn("qwen-websocket-error", command->error);
        break;
    }
}

static esp_err_t send_input_batch(void)
{
    if (!s_session_ready || !s_remote_cleared) return ESP_ERR_INVALID_STATE;
    if (s_pending_count == 0U) {
        while (s_pending_count < QWEN_INPUT_SEND_BLOCKS &&
               xQueueReceive(s_input_queue,
                             &s_pending_indices[s_pending_count], 0) == pdTRUE) {
            qwen_input_block_t *block =
                &s_input_blocks[s_pending_indices[s_pending_count]];
            size_t bytes = block->sample_count * sizeof(int16_t);
            memcpy(s_pending_pcm + s_pending_bytes, block->pcm, bytes);
            s_pending_bytes += bytes;
            ++s_pending_count;
        }
    }
    if (s_pending_count == 0U) return ESP_ERR_NOT_FOUND;
    size_t json_length = 0U;
    esp_err_t err = qwen_protocol_build_audio_append(
        s_tx_json, QWEN_INPUT_JSON_BYTES, s_pending_pcm, s_pending_bytes,
        &json_length);
    if (err == ESP_OK && websocket_send(s_tx_json, json_length) !=
                             (int)json_length) err = ESP_FAIL;
    if (err == ESP_OK) {
        s_mic_pcm_up += s_pending_bytes;
        release_pending_input();
    }
    return err;
}

static void process_input(void)
{
    if (s_state != QWEN_VOICE_CANDIDATE &&
        s_state != QWEN_VOICE_LISTENING) return;
    if (!s_session_ready) return;
    if (!s_remote_cleared) {
        if (send_simple("input_audio_buffer.clear") == ESP_OK)
            s_remote_cleared = true;
        else
            return;
    }
    for (unsigned budget = 0U; budget < 8U; ++budget) {
        esp_err_t err = send_input_batch();
        if (err == ESP_ERR_NOT_FOUND) break;
        if (err != ESP_OK) {
            fail_turn("input-audio-send-failed", err);
            return;
        }
    }
    if (s_speech_end && s_route_confirmed && s_pending_count == 0U &&
        uxQueueMessagesWaiting(s_input_queue) == 0U && !s_commit_sent) {
        if (send_simple("input_audio_buffer.commit") != ESP_OK ||
            send_simple("response.create") != ESP_OK) {
            fail_turn("input-commit-failed", ESP_FAIL);
            return;
        }
        s_commit_sent = true;
        s_response_created = true;
        set_state(QWEN_VOICE_THINKING, "input-committed");
        send_p4_state(QWEN_VOICE_THINKING);
        ESP_LOGI(TAG, "input committed pcm_bytes=%" PRIu64, s_mic_pcm_up);
    }
}

static void start_audio_output(void)
{
    if (s_audio_started) return;
    uint8_t format[6] = {
        (uint8_t)(QWEN_OUTPUT_RATE_HZ & 0xFFU),
        (uint8_t)((QWEN_OUTPUT_RATE_HZ >> 8U) & 0xFFU),
        (uint8_t)((QWEN_OUTPUT_RATE_HZ >> 16U) & 0xFFU),
        (uint8_t)((QWEN_OUTPUT_RATE_HZ >> 24U) & 0xFFU),
        16U, 1U,
    };
    if (voice_uart_link_send_message(VOICE_MSG_AI_AUDIO_START,
                                     format, sizeof(format)) != ESP_OK)
        ++s_uart_errors;
    s_audio_started = true;
    set_state(QWEN_VOICE_SPEAKING, "response.audio.delta");
    send_p4_state(QWEN_VOICE_SPEAKING);
}

static void handle_audio_delta(const qwen_json_span_t *delta)
{
    if (delta->data == NULL || delta->length == 0U) return;
    start_audio_output();
    const uint64_t now = (uint64_t)esp_timer_get_time();
    const uint64_t gap = s_last_audio_us == 0U ? 0U : now - s_last_audio_us;
    if (gap > s_max_audio_gap_us) s_max_audio_gap_us = gap;
    ++s_audio_delta_index;
    size_t decoded_total = 0U;
    size_t offset = 0U;
    while (offset < delta->length) {
        size_t encoded = delta->length - offset;
        if (encoded > 8192U) encoded = 8192U;
        encoded &= ~((size_t)3U);
        if (encoded == 0U) {
            fail_turn("audio-base64-alignment", ESP_ERR_INVALID_SIZE);
            return;
        }
        qwen_json_span_t part = {
            .data = delta->data + offset,
            .length = encoded,
        };
        size_t decoded = 0U;
        esp_err_t err = qwen_protocol_decode_audio(
            &part, s_decode_pcm, QWEN_DECODE_BYTES, &decoded);
        if (err != ESP_OK || output_write_all(s_decode_pcm, decoded) != ESP_OK) {
            fail_turn("audio-delta-decode", err != ESP_OK ? err : ESP_FAIL);
            return;
        }
        decoded_total += decoded;
        offset += encoded;
    }
    s_last_audio_us = now;
    s_ai_pcm_down += decoded_total;
    if (gap > QWEN_AUDIO_GAP_WARN_US)
        ESP_LOGW(TAG, "UPSTREAM_AUDIO_GAP gap=%" PRIu64 "ms", gap / 1000ULL);
    ESP_LOGI("AUDIO_FLOW", "idx=%u bytes=%u gap_ms=%" PRIu64
             " total=%" PRIu64,
             (unsigned)s_audio_delta_index, (unsigned)decoded_total,
             gap / 1000ULL, s_ai_pcm_down);
}

static void send_text_span(const qwen_json_span_t *span)
{
    if (span == NULL || span->data == NULL || span->length == 0U) return;
    size_t length = span->length;
    if (length > VOICE_LINK_MAX_PAYLOAD_BYTES)
        length = VOICE_LINK_MAX_PAYLOAD_BYTES;
    if (voice_uart_link_send_message(VOICE_MSG_AI_TEXT, span->data,
                                     (uint16_t)length) != ESP_OK)
        ++s_uart_errors;
}

static void process_server_message(int slot_index)
{
    qwen_ws_slot_t *slot = &s_ws_slots[slot_index];
    qwen_server_event_t event;
    esp_err_t err = qwen_protocol_parse_server_event(
        (const char *)slot->data, slot->length, &event);
    if (err != ESP_OK) {
        release_ws_slot(slot_index);
        fail_turn("qwen-json-invalid", err);
        return;
    }
    s_last_business_us = (uint64_t)esp_timer_get_time();
    strlcpy(s_last_server_event, event.type, sizeof(s_last_server_event));
    switch (event.kind) {
    case QWEN_SERVER_SESSION_CREATED:
        if (!s_session_update_sent && send_session_update() != ESP_OK)
            fail_turn("session-update-failed", ESP_FAIL);
        break;
    case QWEN_SERVER_SESSION_UPDATED:
        s_session_ready = true;
        ESP_LOGI(TAG, "Qwen session ready model=%s", CONFIG_SMARTSCORE_QWEN_REALTIME_MODEL);
        break;
    case QWEN_SERVER_AUDIO_DELTA:
        handle_audio_delta(&event.delta);
        break;
    case QWEN_SERVER_AUDIO_DONE:
        s_audio_done = true;
        s_output_done_pending = true;
        wake_uart_task();
        set_state(QWEN_VOICE_DRAINING, "response.audio.done");
        ESP_LOGI(TAG, "SERVER_EVENT type=response.audio.done pcm=%" PRIu64,
                 s_ai_pcm_down);
        break;
    case QWEN_SERVER_RESPONSE_DONE:
        s_response_done = true;
        ESP_LOGI(TAG, "SERVER_EVENT type=response.done");
        break;
    case QWEN_SERVER_TEXT_DONE:
        if (event.transcript.data != NULL) send_text_span(&event.transcript);
        else if (event.text.data != NULL) send_text_span(&event.text);
        break;
    case QWEN_SERVER_ERROR: {
        char reason[128] = "qwen-server-error";
        if (event.error_message.data != NULL) {
            size_t copy = event.error_message.length;
            if (copy >= sizeof(reason)) copy = sizeof(reason) - 1U;
            memcpy(reason, event.error_message.data, copy);
            reason[copy] = '\0';
        }
        fail_turn(reason, ESP_FAIL);
        break;
    }
    case QWEN_SERVER_SESSION_CLOSED:
        s_session_ready = false;
        if (s_state != QWEN_VOICE_IDLE)
            fail_turn("qwen-session-closed", ESP_ERR_INVALID_STATE);
        break;
    default:
        break;
    }
    release_ws_slot(slot_index);
}

static void process_messages(void)
{
    for (unsigned budget = 0U; budget < 8U; ++budget) {
        uint8_t slot;
        if (xQueueReceive(s_message_queue, &slot, 0) != pdTRUE) break;
        process_server_message(slot);
    }
}

static void process_completion(void)
{
    const uint64_t now = (uint64_t)esp_timer_get_time();
    if (s_state == QWEN_VOICE_DRAINING) {
        if (s_audio_done && s_output_done_sent &&
            s_drain_wait_started_us == 0U) {
            s_drain_wait_started_us = now;
            ESP_LOGI(TAG,
                     "drain wait started response_done=%u p4_drained=%u",
                     s_response_done ? 1U : 0U, s_p4_drained ? 1U : 0U);
        }
        /* response.audio.done is the definitive end of this turn's PCM.
         * response.done is still recorded, but a missing/late bookkeeping
         * event must not keep local WakeNet disabled after P4 has drained. */
        if (s_audio_done && s_output_done_sent && s_p4_drained) {
            ESP_LOGI(TAG,
                     "audio turn drained response_done=%u; restoring wake",
                     s_response_done ? 1U : 0U);
            finish_to_idle("audio-complete-and-drained");
            return;
        }
        if (s_drain_wait_started_us != 0U &&
            now - s_drain_wait_started_us >= QWEN_DRAIN_TIMEOUT_US) {
            ESP_LOGW(TAG,
                     "drain completion timeout; forcing wake recovery "
                     "audio_done=%u response_done=%u uart_done=%u p4_drained=%u",
                     s_audio_done ? 1U : 0U, s_response_done ? 1U : 0U,
                     s_output_done_sent ? 1U : 0U, s_p4_drained ? 1U : 0U);
            finish_to_idle("drain-timeout-recovery");
            return;
        }
    }
    if ((s_state == QWEN_VOICE_THINKING ||
         s_state == QWEN_VOICE_SPEAKING) && s_last_business_us != 0U &&
        now - s_last_business_us > QWEN_STALL_TIMEOUT_US &&
        output_buffered() == 0U) {
        fail_turn("qwen-business-stalled", ESP_ERR_TIMEOUT);
    }
}

static void log_summary(void)
{
    uint64_t now = (uint64_t)esp_timer_get_time();
    if (now < s_next_summary_us) return;
    s_next_summary_us = now + QWEN_SUMMARY_INTERVAL_US;
    ESP_LOGI(TAG,
             "state=%s ws=%s mic_pcm_up=%" PRIu64
             " ai_pcm_down=%" PRIu64 " uart_pcm_sent=%" PRIu64
             " qwen_rx_buffer=%u uart_tx_buffer=%u/%u max_audio_gap=%" PRIu64
             "ms uart_errors=%u input_drops=%u ws_rx=%" PRIu64
             " callback_max=%" PRIu64
             "us done[audio=%u response=%u uart=%u p4=%u]",
             state_name(s_state), s_ws_connected ? "UP" : "DOWN",
             s_mic_pcm_up, s_ai_pcm_down, s_uart_pcm_sent,
             (unsigned)uxQueueMessagesWaiting(s_message_queue),
             (unsigned)output_buffered(), (unsigned)QWEN_OUTPUT_RING_BYTES,
             s_max_audio_gap_us / 1000ULL, (unsigned)s_uart_errors,
             (unsigned)s_input_drops, s_ws_rx_events, s_ws_callback_max_us,
             s_audio_done ? 1U : 0U, s_response_done ? 1U : 0U,
             s_output_done_sent ? 1U : 0U, s_p4_drained ? 1U : 0U);
}

static void qwen_pcm_uart_task(void *argument)
{
    (void)argument;
    uint8_t pcm[VOICE_LINK_PCM_PAYLOAD_BYTES];
    while (true) {
        if (!s_uart_flow_enabled || s_stop_requested) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        size_t bytes = output_read(pcm, sizeof(pcm));
        if (bytes > 0U) {
            esp_err_t err;
            do {
                err = voice_uart_link_send_message(
                    VOICE_MSG_AI_AUDIO_PCM, pcm, (uint16_t)bytes);
                if (err != ESP_OK) {
                    ++s_uart_errors;
                    vTaskDelay(pdMS_TO_TICKS(2));
                }
            } while (err != ESP_OK && !s_stop_requested &&
                     s_uart_flow_enabled);
            if (err == ESP_OK) s_uart_pcm_sent += bytes;
            continue;
        }
        if (s_output_done_pending && !s_output_done_sent) {
            if (voice_uart_link_send_message(
                    VOICE_MSG_AI_AUDIO_DONE, NULL, 0U) == ESP_OK) {
                s_output_done_sent = true;
                s_output_done_pending = false;
                ESP_LOGI(TAG, "AI_AUDIO_DONE sent to P4");
                if (s_owner_task != NULL) xTaskNotifyGive(s_owner_task);
            } else {
                ++s_uart_errors;
            }
        }
        const TickType_t wait_ticks =
            s_output_done_pending && !s_output_done_sent
                ? pdMS_TO_TICKS(100)
                : portMAX_DELAY;
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
    }
}

static void qwen_voice_task(void *argument)
{
    (void)argument;
    while (true) {
        qwen_command_t command;
        while (xQueueReceive(s_command_queue, &command, 0) == pdTRUE) {
            handle_command(&command);
            memset(command.password, 0, sizeof(command.password));
        }
        if (s_wifi_got_ip && s_ws_client == NULL &&
            CONFIG_SMARTSCORE_QWEN_REALTIME_ENABLED &&
            QWEN_S3_API_KEY[0] != '\0') {
            int uri_length = snprintf(s_ws_uri, sizeof(s_ws_uri), "%s?model=%s",
                                      CONFIG_SMARTSCORE_QWEN_REALTIME_ENDPOINT,
                                      CONFIG_SMARTSCORE_QWEN_REALTIME_MODEL);
            int header_length = snprintf(s_ws_headers, sizeof(s_ws_headers),
                                         "Authorization: Bearer %s\r\n",
                                         QWEN_S3_API_KEY);
            if (uri_length > 0 && uri_length < (int)sizeof(s_ws_uri) &&
                header_length > 0 && header_length < (int)sizeof(s_ws_headers)) {
                const esp_websocket_client_config_t config = {
                    .uri = s_ws_uri,
                    .disable_auto_reconnect = false,
                    .task_prio = 7,
                    .task_name = "qwen_s3_ws",
                    .task_stack = QWEN_WS_TASK_STACK_BYTES,
                    .buffer_size = QWEN_WS_BUFFER_BYTES,
                    .headers = s_ws_headers,
                    .crt_bundle_attach = esp_crt_bundle_attach,
                    .ping_interval_sec = 10,
                    .reconnect_timeout_ms = 3000,
                    .network_timeout_ms = 10000,
                };
                s_ws_client = esp_websocket_client_init(&config);
                if (s_ws_client != NULL) {
                    esp_err_t err = esp_websocket_register_events(
                        s_ws_client, WEBSOCKET_EVENT_ANY,
                        qwen_websocket_event_handler, NULL);
                    if (err == ESP_OK) err = esp_websocket_client_start(s_ws_client);
                    if (err != ESP_OK) {
                        (void)esp_websocket_client_destroy(s_ws_client);
                        s_ws_client = NULL;
                    } else {
                        s_ws_starting = true;
                        ESP_LOGI(TAG, "connecting warm Qwen WebSocket model=%s",
                                 CONFIG_SMARTSCORE_QWEN_REALTIME_MODEL);
                    }
                }
            }
        }
        process_messages();
        process_input();
        process_completion();
        log_summary();
        const TickType_t wait_ticks = s_state == QWEN_VOICE_IDLE
                                          ? pdMS_TO_TICKS(250)
                                          : pdMS_TO_TICKS(10);
        (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
    }
}

static void websocket_data_event(const esp_websocket_event_data_t *data)
{
    if (data == NULL || data->op_code >= 0x08U) return;
    if (data->data_len < 0 || data->payload_len < 0 ||
        data->payload_offset < 0 ||
        (data->data_len > 0 && data->data_ptr == NULL)) {
        reset_assembler();
        return;
    }
    const size_t chunk = (size_t)data->data_len;
    const size_t offset = (size_t)data->payload_offset;
    const size_t frame_length = data->payload_len > 0
                                    ? (size_t)data->payload_len : chunk;
    if (offset == 0U) {
        if (data->op_code == 0x01U || data->op_code == 0x02U) {
            if (s_assembler.active) reset_assembler();
            int slot = allocate_ws_slot();
            if (slot < 0) return;
            s_assembler.active = true;
            s_assembler.slot = slot;
            s_assembler.opcode = data->op_code;
        } else if (data->op_code != 0x00U || !s_assembler.active) {
            reset_assembler();
            return;
        }
        s_assembler.frame_expected = frame_length;
        s_assembler.frame_received = 0U;
    } else if (!s_assembler.active) {
        return;
    }
    if (offset != s_assembler.frame_received || offset > frame_length ||
        chunk > frame_length - offset) {
        reset_assembler();
        return;
    }
    qwen_ws_slot_t *slot = &s_ws_slots[s_assembler.slot];
    if (slot->length + chunk > QWEN_WS_SLOT_BYTES) {
        reset_assembler();
        return;
    }
    if (chunk > 0U) {
        memcpy(slot->data + slot->length, data->data_ptr, chunk);
        slot->length += chunk;
        s_assembler.frame_received += chunk;
    }
    if (s_assembler.frame_received != s_assembler.frame_expected) return;
    s_assembler.frame_expected = 0U;
    s_assembler.frame_received = 0U;
    if (!data->fin) return;
    slot->data[slot->length] = '\0';
    uint8_t complete = (uint8_t)s_assembler.slot;
    memset(&s_assembler, 0, sizeof(s_assembler));
    s_assembler.slot = -1;
    if (xQueueSend(s_message_queue, &complete, 0) != pdTRUE)
        release_ws_slot(complete);
    else if (s_owner_task != NULL)
        xTaskNotifyGive(s_owner_task);
}

static void qwen_websocket_event_handler(void *handler_arg,
                                         esp_event_base_t base,
                                         int32_t event_id,
                                         void *event_data)
{
    (void)handler_arg;
    (void)base;
    esp_websocket_event_data_t *data = event_data;
    qwen_command_t command = {0};
    switch ((esp_websocket_event_id_t)event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        reset_assembler();
        command.type = CMD_WS_CONNECTED;
        (void)queue_command(&command);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        reset_assembler();
        command.type = CMD_WS_DISCONNECTED;
        (void)queue_command(&command);
        break;
    case WEBSOCKET_EVENT_ERROR:
        command.type = CMD_WS_ERROR;
        command.error = data != NULL &&
                                data->error_handle.esp_tls_last_esp_err != 0
                            ? data->error_handle.esp_tls_last_esp_err
                            : ESP_FAIL;
        (void)queue_command(&command);
        break;
    case WEBSOCKET_EVENT_DATA: {
        const uint64_t start = (uint64_t)esp_timer_get_time();
        ++s_ws_rx_events;
        if (data != NULL && data->data_len > 0)
            s_ws_rx_bytes += (uint64_t)data->data_len;
        websocket_data_event(data);
        const uint64_t elapsed = (uint64_t)esp_timer_get_time() - start;
        if (elapsed > s_ws_callback_max_us) s_ws_callback_max_us = elapsed;
        if (elapsed > QWEN_CALLBACK_WARN_US)
            ESP_LOGW(TAG, "slow websocket callback %" PRIu64 " us", elapsed);
        break;
    }
    default:
        break;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    (void)data;
    qwen_command_t command = {0};
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        command.type = CMD_WIFI_UP;
        (void)queue_command(&command);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        command.type = CMD_WIFI_DOWN;
        (void)queue_command(&command);
        if (s_wifi_have_credentials) (void)esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START &&
               s_wifi_have_credentials) {
        (void)esp_wifi_connect();
    }
}

static esp_err_t initialize_wifi(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    if (esp_netif_create_default_wifi_sta() == NULL) return ESP_ERR_NO_MEM;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     wifi_event_handler, NULL);
    if (err == ESP_OK)
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                         wifi_event_handler, NULL);
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err == ESP_OK) err = esp_wifi_start();
    return err;
}

static void load_saved_wifi(void)
{
    nvs_handle_t nvs = 0;
    if (nvs_open("qwen_wifi", NVS_READONLY, &nvs) != ESP_OK) return;
    qwen_command_t command = {.type = CMD_WIFI_CREDENTIALS};
    size_t ssid_size = sizeof(command.ssid);
    size_t password_size = sizeof(command.password);
    esp_err_t err = nvs_get_str(nvs, "ssid", command.ssid, &ssid_size);
    if (err == ESP_OK)
        err = nvs_get_str(nvs, "password", command.password, &password_size);
    nvs_close(nvs);
    if (err == ESP_OK && command.ssid[0] != '\0')
        (void)queue_command(&command);
    memset(command.password, 0, sizeof(command.password));
}

esp_err_t qwen_realtime_init(void)
{
    if (s_initialized) return ESP_OK;
    if (!CONFIG_SMARTSCORE_QWEN_REALTIME_ENABLED) return ESP_ERR_NOT_SUPPORTED;

    s_command_queue = xQueueCreate(QWEN_COMMAND_QUEUE_LENGTH,
                                   sizeof(qwen_command_t));
    s_input_queue = xQueueCreate(QWEN_INPUT_BLOCK_COUNT, sizeof(uint16_t));
    s_message_queue = xQueueCreate(QWEN_WS_SLOT_COUNT, sizeof(uint8_t));
    s_output_lock = xSemaphoreCreateMutex();
    s_input_blocks = heap_caps_calloc(QWEN_INPUT_BLOCK_COUNT,
                                      sizeof(*s_input_blocks),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pending_pcm = heap_caps_malloc(
        QWEN_INPUT_SEND_BLOCKS * QWEN_INPUT_BLOCK_SAMPLES * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_output_ring = heap_caps_malloc(QWEN_OUTPUT_RING_BYTES,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_tx_json = heap_caps_malloc(QWEN_INPUT_JSON_BYTES,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_decode_pcm = heap_caps_malloc(QWEN_DECODE_BYTES,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /*
     * This task persists Wi-Fi credentials through NVS. Flash operations can
     * disable the external-memory cache, so its live stack must stay in
     * internal RAM even though all large audio/message buffers remain in
     * PSRAM.
     */
    s_owner_stack = heap_caps_malloc(QWEN_OWNER_STACK_BYTES,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    for (size_t i = 0U; i < QWEN_WS_SLOT_COUNT; ++i)
        s_ws_slots[i].data = heap_caps_malloc(
            QWEN_WS_SLOT_BYTES + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool memory_ok = s_command_queue != NULL && s_input_queue != NULL &&
                     s_message_queue != NULL && s_output_lock != NULL &&
                     s_input_blocks != NULL && s_pending_pcm != NULL &&
                     s_output_ring != NULL && s_tx_json != NULL &&
                     s_decode_pcm != NULL && s_owner_stack != NULL;
    for (size_t i = 0U; i < QWEN_WS_SLOT_COUNT; ++i)
        memory_ok = memory_ok && s_ws_slots[i].data != NULL;
    if (!memory_ok) return ESP_ERR_NO_MEM;

    for (uint16_t i = 0U; i < QWEN_INPUT_BLOCK_COUNT; ++i)
        s_input_free[s_input_free_count++] = i;
    s_initialized = true;
    esp_err_t err = initialize_wifi();
    if (err != ESP_OK) return err;
    s_owner_task = xTaskCreateStatic(
        qwen_voice_task, "qwen_voice_task",
        QWEN_OWNER_STACK_BYTES / sizeof(StackType_t), NULL, 8,
        s_owner_stack, &s_owner_tcb);
    if (s_owner_task == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(qwen_pcm_uart_task, "qwen_pcm_uart",
                    QWEN_UART_STACK_BYTES, NULL, 7, &s_uart_task) != pdPASS)
        return ESP_ERR_NO_MEM;
    load_saved_wifi();
    if (QWEN_S3_API_KEY[0] == '\0')
        ESP_LOGE(TAG, "DASHSCOPE_API_KEY missing; local voice commands remain available");
    ESP_LOGI(TAG,
             "initialized model=%s input=16000/16/1 output=24000/16/1 output_ring=%u UART=921600 API-key=%s",
             CONFIG_SMARTSCORE_QWEN_REALTIME_MODEL,
             (unsigned)QWEN_OUTPUT_RING_BYTES,
             QWEN_S3_API_KEY[0] != '\0' ? "configured" : "missing");
    return ESP_OK;
}

esp_err_t qwen_realtime_set_wifi_credentials(const char *ssid,
                                              const char *password)
{
    if (!s_initialized || ssid == NULL || password == NULL ||
        ssid[0] == '\0' || strlen(ssid) > 32U || strlen(password) > 64U)
        return ESP_ERR_INVALID_ARG;
    qwen_command_t command = {.type = CMD_WIFI_CREDENTIALS};
    strlcpy(command.ssid, ssid, sizeof(command.ssid));
    strlcpy(command.password, password, sizeof(command.password));
    bool queued = queue_command(&command);
    memset(command.password, 0, sizeof(command.password));
    return queued ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t qwen_realtime_arm(void)
{
    qwen_command_t command = {.type = CMD_ARM};
    return queue_command(&command) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t qwen_realtime_begin(void)
{
    qwen_command_t command = {.type = CMD_BEGIN};
    return queue_command(&command) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t qwen_realtime_speech_end(void)
{
    qwen_command_t command = {.type = CMD_SPEECH_END};
    return queue_command(&command) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void qwen_realtime_cancel_local(void)
{
    qwen_command_t command = {.type = CMD_LOCAL_CANCEL};
    (void)queue_command(&command);
}

void qwen_realtime_request_stop(void)
{
    s_stop_requested = true;
    wake_uart_task();
    qwen_command_t command = {.type = CMD_STOP};
    (void)queue_command(&command);
}

void qwen_realtime_set_uart_flow(bool enabled)
{
    s_uart_flow_enabled = enabled;
    wake_uart_task();
    ESP_LOGI(TAG, "P4 UART flow %s buffered=%u", enabled ? "ON" : "OFF",
             (unsigned)output_buffered());
}

void qwen_realtime_notify_p4_drained(void)
{
    qwen_command_t command = {.type = CMD_P4_DRAINED};
    (void)queue_command(&command);
}

esp_err_t qwen_realtime_push_pcm(const int16_t *pcm, size_t sample_count)
{
    if (!s_initialized || pcm == NULL || sample_count == 0U ||
        sample_count > QWEN_INPUT_BLOCK_SAMPLES) return ESP_ERR_INVALID_ARG;
    if (!s_accept_pcm) return ESP_ERR_INVALID_STATE;
    uint16_t index;
    if (!allocate_input(&index)) {
        ++s_input_drops;
        return ESP_ERR_TIMEOUT;
    }
    qwen_input_block_t *block = &s_input_blocks[index];
    block->sample_count = (uint16_t)sample_count;
    memcpy(block->pcm, pcm, sample_count * sizeof(int16_t));
    if (xQueueSend(s_input_queue, &index, 0) != pdTRUE) {
        release_input(index);
        ++s_input_drops;
        return ESP_ERR_TIMEOUT;
    }
    if (s_owner_task != NULL) xTaskNotifyGive(s_owner_task);
    return ESP_OK;
}

qwen_voice_state_t qwen_realtime_state(void)
{
    return s_state;
}
