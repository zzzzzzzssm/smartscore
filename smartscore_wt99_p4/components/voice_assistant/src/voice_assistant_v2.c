#include "voice_assistant_v2.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "speaker_service.h"
#include "voice_v2_protocol.h"

#ifndef CONFIG_SMARTSCORE_QWEN_AUDIO_REALTIME_ASSISTANT
#define CONFIG_SMARTSCORE_QWEN_AUDIO_REALTIME_ASSISTANT 0
#endif
#ifndef CONFIG_SMARTSCORE_QWEN_AUDIO_REALTIME_ENDPOINT
#define CONFIG_SMARTSCORE_QWEN_AUDIO_REALTIME_ENDPOINT \
    "wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen-audio-3.0-realtime-plus"
#endif
#ifndef CONFIG_SMARTSCORE_QWEN_AUDIO_REALTIME_VOICE
#define CONFIG_SMARTSCORE_QWEN_AUDIO_REALTIME_VOICE "longanqian"
#endif
#ifndef CONFIG_SMARTSCORE_QWEN_AUDIO_REALTIME_INSTRUCTIONS
#define CONFIG_SMARTSCORE_QWEN_AUDIO_REALTIME_INSTRUCTIONS \
    "You are the SmartScore piano voice assistant. Answer naturally and concisely in Chinese."
#endif
#ifndef CONFIG_SMARTSCORE_QWEN_AUDIO_REALTIME_RECONNECT_MS
#define CONFIG_SMARTSCORE_QWEN_AUDIO_REALTIME_RECONNECT_MS 3000
#endif
#ifndef QWEN_AUDIO_API_KEY
#define QWEN_AUDIO_API_KEY ""
#endif

#define VOICE_V2_INPUT_RATE_HZ 16000U
#define VOICE_V2_OUTPUT_RATE_HZ 24000U
#define VOICE_V2_PCM_BLOCK_SAMPLES 320U
#define VOICE_V2_PCM_BLOCK_COUNT 192U
#define VOICE_V2_SEND_BLOCKS 8U
#define VOICE_V2_SEND_PCM_BYTES \
    (VOICE_V2_PCM_BLOCK_SAMPLES * sizeof(int16_t) * VOICE_V2_SEND_BLOCKS)
#define VOICE_V2_TX_JSON_BYTES 8192U
#define VOICE_V2_DECODE_BYTES 6144U
#define VOICE_V2_WS_SLOT_COUNT 8U
#define VOICE_V2_WS_SLOT_BYTES (192U * 1024U)
#define VOICE_V2_COMMAND_QUEUE_LENGTH 32U
#define VOICE_V2_MESSAGE_QUEUE_LENGTH VOICE_V2_WS_SLOT_COUNT
#define VOICE_V2_TASK_STACK_BYTES 14336U
#define VOICE_V2_TASK_PRIORITY 8U
#define VOICE_V2_TASK_TICK_MS 10U
#define VOICE_V2_SERVER_WORK_BUDGET 64U
#define VOICE_V2_WS_TASK_STACK_BYTES 6144U
#define VOICE_V2_WS_TASK_PRIORITY 7U
#define VOICE_V2_WS_BUFFER_BYTES 4096U
#define VOICE_V2_SEND_TIMEOUT_MS 1500U
#define VOICE_V2_INPUT_SEND_GAP_US 20000ULL
#define VOICE_V2_FAST_FLUSH_QUEUE_BLOCKS 4U
#define VOICE_V2_INPUT_RETRY_GAP_US 100000ULL
#define VOICE_V2_INPUT_SEND_RETRIES 2U
#define VOICE_V2_INPUT_SLOW_WARN_US 200000ULL
#define VOICE_V2_CANDIDATE_TIMEOUT_MS 2800ULL
#define VOICE_V2_MIN_SPEECH_FRAMES 8U
#define VOICE_V2_SPEECH_LEVEL 180U
#define VOICE_V2_SUMMARY_INTERVAL_US 1000000ULL
#define VOICE_V2_AUDIO_GAP_WARN_US 800000ULL
#define VOICE_V2_CALLBACK_WARN_US 20000ULL
#define VOICE_V2_SPEAKING_STALL_US 8000000ULL
#define VOICE_V2_SPEAKER_ABORT_GRACE_MS 200U
#define VOICE_V2_CONTEXT_BYTES 512U
#define VOICE_V2_INSTRUCTIONS_BYTES 1536U
#define VOICE_V2_REASON_BYTES 64U

typedef enum {
    VOICE_V2_STATE_IDLE = 0,
    VOICE_V2_STATE_CANDIDATE,
    VOICE_V2_STATE_LISTENING,
    VOICE_V2_STATE_WAIT_RESPONSE,
    VOICE_V2_STATE_SPEAKING,
    VOICE_V2_STATE_DRAINING,
    VOICE_V2_STATE_CLOSING,
} voice_v2_state_t;

typedef enum {
    VOICE_V2_COMMAND_WAKE = 0,
    VOICE_V2_COMMAND_AI_BEGIN,
    VOICE_V2_COMMAND_SPEECH_END,
    VOICE_V2_COMMAND_LOCAL_CANCEL,
    VOICE_V2_COMMAND_NETWORK_READY,
    VOICE_V2_COMMAND_WS_CONNECTED,
    VOICE_V2_COMMAND_WS_DISCONNECTED,
    VOICE_V2_COMMAND_WS_ERROR,
    VOICE_V2_COMMAND_WS_CLOSED,
    VOICE_V2_COMMAND_WS_PROTOCOL_ERROR,
} voice_v2_command_type_t;

typedef struct {
    voice_v2_command_type_t type;
    bool flag;
    esp_err_t error;
    char reason[VOICE_V2_REASON_BYTES];
} voice_v2_command_t;

typedef struct {
    uint16_t sequence;
    uint16_t sample_count;
    bool speech;
    int16_t pcm[VOICE_V2_PCM_BLOCK_SAMPLES];
} voice_v2_pcm_block_t;

typedef struct {
    uint8_t *data;
    size_t length;
    uint64_t completed_us;
    uint8_t opcode;
    bool in_use;
} voice_v2_ws_slot_t;

typedef struct {
    int slot;
    size_t frame_expected;
    size_t frame_received;
    uint8_t opcode;
    bool active;
} voice_v2_ws_assembler_t;

typedef struct {
    uint64_t rx_events;
    uint64_t rx_bytes;
    uint64_t last_rx_us;
    uint64_t callback_max_us;
} voice_v2_ws_metrics_t;

typedef struct {
    uint64_t wake_us;
    uint64_t session_created_us;
    uint64_t ai_begin_us;
    uint64_t speech_end_us;
    uint64_t input_commit_us;
    uint64_t first_asr_us;
    uint64_t asr_done_us;
    uint64_t audio_started_us;
    uint64_t first_pcm_us;
    uint64_t speaker_started_us;
    uint64_t audio_done_us;
    uint64_t response_done_us;
    uint64_t playback_done_us;
} voice_v2_latency_t;

static const char *TAG = "VOICE_V2";
static bool s_initialized;
static volatile bool s_accept_pcm;
static bool s_network_ready;
static bool s_ws_connected;
static bool s_ws_starting;
static bool s_route_confirmed;
static bool s_session_update_sent;
static bool s_session_ready;
static bool s_speech_end_received;
static bool s_input_commit_sent;
static bool s_input_commit_ack;
static bool s_response_create_sent;
static bool s_ignore_cancelled_response;
static bool s_audio_stream_started;
static bool s_audio_stream_held;
static bool s_audio_done;
static bool s_response_done;
static bool s_finish_sent;
static bool s_speaker_was_active;
static bool s_completion_reported;
static bool s_local_cancel;
static bool s_stall_drain_requested;
static bool s_candidate_failure;
static esp_err_t s_candidate_failure_error;
static voice_v2_state_t s_state = VOICE_V2_STATE_IDLE;
static uint64_t s_state_enter_us;
static uint64_t s_finish_sent_us;
static uint64_t s_next_summary_us;
static uint64_t s_last_business_rx_us;
static uint64_t s_last_response_business_rx_us;
static uint64_t s_last_audio_delta_us;
static uint64_t s_audio_flow_start_us;
static uint64_t s_next_input_send_us;
static uint64_t s_pcm_up_bytes;
static uint64_t s_pcm_input_bytes;
static uint64_t s_pcm_down_bytes;
static uint64_t s_audio_flow_bytes;
static uint64_t s_business_events;
static uint32_t s_audio_delta_index;
static uint32_t s_candidate_speech_frames;
static uint8_t s_input_send_retries;
static uint32_t s_fast_flush_batches;
static volatile uint32_t s_observed_speech_frames;
static uint32_t s_pcm_pool_exhaustions;
static char s_last_server_event[96];
static char s_last_client_event[96];
static char s_stop_reason[VOICE_V2_REASON_BYTES];
static voice_v2_latency_t s_latency;

static QueueHandle_t s_command_queue;
static QueueHandle_t s_pcm_ready_queue;
static QueueHandle_t s_message_queue;
static voice_v2_pcm_block_t *s_pcm_blocks;
static uint16_t s_pcm_free_stack[VOICE_V2_PCM_BLOCK_COUNT];
static size_t s_pcm_free_count;
static portMUX_TYPE s_pcm_pool_lock = portMUX_INITIALIZER_UNLOCKED;
static voice_v2_ws_slot_t s_ws_slots[VOICE_V2_WS_SLOT_COUNT];
static voice_v2_ws_assembler_t s_ws_assembler = {.slot = -1};
static portMUX_TYPE s_ws_slot_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_ws_slots_in_use;
static uint32_t s_ws_slots_max_in_use;
static uint32_t s_ws_slot_exhaustions;
static portMUX_TYPE s_ws_metrics_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_ws_rx_events;
static uint64_t s_ws_rx_bytes;
static uint64_t s_last_ws_rx_us;
static uint64_t s_ws_callback_max_us;
static int s_active_message = -1;
static voice_v2_server_event_t s_active_event;
static size_t s_audio_encoded_offset;
static size_t s_audio_decoded_bytes;
static size_t s_audio_decoded_offset;
static size_t s_active_delta_pcm_bytes;
static uint64_t s_active_audio_rx_us;
static uint64_t s_active_audio_gap_us;
static uint32_t s_active_audio_delta_index;

static uint8_t *s_send_pcm;
static uint16_t s_pending_input_indices[VOICE_V2_SEND_BLOCKS];
static size_t s_pending_input_block_count;
static size_t s_pending_input_pcm_bytes;
static char *s_tx_json;
static uint8_t *s_decode_pcm;
static StackType_t *s_task_stack;
static StaticTask_t s_task_control;
static TaskHandle_t s_task;
static esp_websocket_client_handle_t s_ws_client;
static char s_ws_headers[384];

static voice_assistant_v2_event_handler_t s_event_handler;
static void *s_event_context;
static voice_assistant_v2_context_provider_t s_context_provider;
static void *s_context_context;

static const char *state_name(voice_v2_state_t state)
{
    switch (state) {
    case VOICE_V2_STATE_IDLE: return "IDLE";
    case VOICE_V2_STATE_CANDIDATE: return "CANDIDATE";
    case VOICE_V2_STATE_LISTENING: return "LISTENING";
    case VOICE_V2_STATE_WAIT_RESPONSE: return "WAIT_RESPONSE";
    case VOICE_V2_STATE_SPEAKING: return "SPEAKING";
    case VOICE_V2_STATE_DRAINING: return "DRAINING";
    case VOICE_V2_STATE_CLOSING: return "CLOSING";
    default: return "UNKNOWN";
    }
}

static void set_state(voice_v2_state_t next, const char *reason)
{
    if (s_state == next) return;
    ESP_LOGI(TAG, "state %s -> %s reason=%s", state_name(s_state),
             state_name(next), reason != NULL ? reason : "-");
    s_state = next;
    s_state_enter_us = (uint64_t)esp_timer_get_time();
}

static void report_event(voice_assistant_v2_event_type_t type,
                         esp_err_t error,
                         const char *reason)
{
    if (s_event_handler == NULL) return;
    const voice_assistant_v2_event_t event = {
        .type = type,
        .error = error,
        .reason = reason,
    };
    s_event_handler(&event, s_event_context);
}

static void mark_once(uint64_t *field)
{
    if (field != NULL && *field == 0U) {
        *field = (uint64_t)esp_timer_get_time();
    }
}

static long long elapsed_ms(uint64_t start, uint64_t end)
{
    if (start == 0U || end == 0U || end < start) return -1LL;
    return (long long)((end - start) / 1000ULL);
}

static long long age_ms(uint64_t now_us, uint64_t event_us)
{
    if (event_us == 0U || now_us < event_us) return -1LL;
    return (long long)((now_us - event_us) / 1000ULL);
}

static bool is_response_event(const char *type)
{
    static const char prefix[] = "response.";
    return type != NULL &&
           strncmp(type, prefix, sizeof(prefix) - 1U) == 0;
}

static bool span_equals(const voice_v2_json_span_t *span,
                        const char *value)
{
    return span != NULL && span->data != NULL && value != NULL &&
           span->length == strlen(value) &&
           memcmp(span->data, value, span->length) == 0;
}

static void get_ws_metrics(voice_v2_ws_metrics_t *metrics)
{
    if (metrics == NULL) return;
    portENTER_CRITICAL(&s_ws_metrics_lock);
    metrics->rx_events = s_ws_rx_events;
    metrics->rx_bytes = s_ws_rx_bytes;
    metrics->last_rx_us = s_last_ws_rx_us;
    metrics->callback_max_us = s_ws_callback_max_us;
    portEXIT_CRITICAL(&s_ws_metrics_lock);
}

static void log_latency(void)
{
    ESP_LOGI(TAG,
             "LATENCY wake=0 session_created=%lld ai_begin=%lld speech_end=%lld input_commit=%lld first_asr=%lld asr_done=%lld audio_started=%lld first_pcm=%lld speaker_started=%lld audio_done=%lld response_done=%lld playback_done=%lld post_speech_first_pcm=%lld post_speech_speaker=%lld ms",
             elapsed_ms(s_latency.wake_us, s_latency.session_created_us),
             elapsed_ms(s_latency.wake_us, s_latency.ai_begin_us),
             elapsed_ms(s_latency.wake_us, s_latency.speech_end_us),
             elapsed_ms(s_latency.wake_us, s_latency.input_commit_us),
             elapsed_ms(s_latency.wake_us, s_latency.first_asr_us),
             elapsed_ms(s_latency.wake_us, s_latency.asr_done_us),
             elapsed_ms(s_latency.wake_us, s_latency.audio_started_us),
             elapsed_ms(s_latency.wake_us, s_latency.first_pcm_us),
             elapsed_ms(s_latency.wake_us, s_latency.speaker_started_us),
             elapsed_ms(s_latency.wake_us, s_latency.audio_done_us),
             elapsed_ms(s_latency.wake_us, s_latency.response_done_us),
             elapsed_ms(s_latency.wake_us, s_latency.playback_done_us),
             elapsed_ms(s_latency.speech_end_us, s_latency.first_pcm_us),
             elapsed_ms(s_latency.speech_end_us,
                        s_latency.speaker_started_us));
}

static bool queue_command(voice_v2_command_type_t type,
                          bool flag,
                          esp_err_t error,
                          const char *reason)
{
    if (s_command_queue == NULL) return false;
    voice_v2_command_t command = {
        .type = type,
        .flag = flag,
        .error = error,
    };
    if (reason != NULL) {
        strlcpy(command.reason, reason, sizeof(command.reason));
    }
    if (xQueueSend(s_command_queue, &command, 0) != pdTRUE) {
        ESP_LOGE(TAG, "control queue full type=%d", (int)type);
        return false;
    }
    if (s_task != NULL) xTaskNotifyGive(s_task);
    return true;
}

static int allocate_ws_slot(void)
{
    int slot = -1;
    portENTER_CRITICAL(&s_ws_slot_lock);
    for (unsigned i = 0U; i < VOICE_V2_WS_SLOT_COUNT; ++i) {
        if (!s_ws_slots[i].in_use) {
            s_ws_slots[i].in_use = true;
            s_ws_slots[i].length = 0U;
            slot = (int)i;
            ++s_ws_slots_in_use;
            if (s_ws_slots_in_use > s_ws_slots_max_in_use) {
                s_ws_slots_max_in_use = s_ws_slots_in_use;
            }
            break;
        }
    }
    if (slot < 0) ++s_ws_slot_exhaustions;
    portEXIT_CRITICAL(&s_ws_slot_lock);
    return slot;
}

static void release_ws_slot(int slot)
{
    if (slot < 0 || slot >= (int)VOICE_V2_WS_SLOT_COUNT) return;
    portENTER_CRITICAL(&s_ws_slot_lock);
    if (s_ws_slots[slot].in_use) {
        s_ws_slots[slot].length = 0U;
        s_ws_slots[slot].opcode = 0U;
        s_ws_slots[slot].completed_us = 0U;
        s_ws_slots[slot].in_use = false;
        if (s_ws_slots_in_use > 0U) --s_ws_slots_in_use;
    }
    portEXIT_CRITICAL(&s_ws_slot_lock);
}

static void get_ws_slot_metrics(uint32_t *in_use,
                                uint32_t *max_in_use,
                                uint32_t *exhaustions)
{
    portENTER_CRITICAL(&s_ws_slot_lock);
    if (in_use != NULL) *in_use = s_ws_slots_in_use;
    if (max_in_use != NULL) *max_in_use = s_ws_slots_max_in_use;
    if (exhaustions != NULL) *exhaustions = s_ws_slot_exhaustions;
    portEXIT_CRITICAL(&s_ws_slot_lock);
}

static void reset_ws_assembler(void)
{
    if (s_ws_assembler.active) release_ws_slot(s_ws_assembler.slot);
    memset(&s_ws_assembler, 0, sizeof(s_ws_assembler));
    s_ws_assembler.slot = -1;
}

static void drain_message_queue(void)
{
    if (s_active_message >= 0) {
        release_ws_slot(s_active_message);
        s_active_message = -1;
    }
    uint8_t slot = 0U;
    while (s_message_queue != NULL &&
           xQueueReceive(s_message_queue, &slot, 0) == pdTRUE) {
        release_ws_slot((int)slot);
    }
    memset(&s_active_event, 0, sizeof(s_active_event));
    s_audio_encoded_offset = 0U;
    s_audio_decoded_bytes = 0U;
    s_audio_decoded_offset = 0U;
    s_active_delta_pcm_bytes = 0U;
}

static bool allocate_pcm_block(uint16_t *out_index)
{
    if (out_index == NULL) return false;
    bool ok = false;
    portENTER_CRITICAL(&s_pcm_pool_lock);
    if (s_pcm_free_count > 0U) {
        *out_index = s_pcm_free_stack[--s_pcm_free_count];
        ok = true;
    }
    portEXIT_CRITICAL(&s_pcm_pool_lock);
    return ok;
}

static void release_pcm_block(uint16_t index)
{
    if (index >= VOICE_V2_PCM_BLOCK_COUNT) return;
    portENTER_CRITICAL(&s_pcm_pool_lock);
    if (s_pcm_free_count < VOICE_V2_PCM_BLOCK_COUNT) {
        s_pcm_free_stack[s_pcm_free_count++] = index;
    }
    portEXIT_CRITICAL(&s_pcm_pool_lock);
}

static uint64_t pcm_input_total(void)
{
    uint64_t total;
    portENTER_CRITICAL(&s_pcm_pool_lock);
    total = s_pcm_input_bytes;
    portEXIT_CRITICAL(&s_pcm_pool_lock);
    return total;
}

static void drain_pcm_queue(void)
{
    uint16_t index = 0U;
    while (s_pcm_ready_queue != NULL &&
           xQueueReceive(s_pcm_ready_queue, &index, 0) == pdTRUE) {
        release_pcm_block(index);
    }
}

static void release_pending_input_batch(void)
{
    for (size_t i = 0U; i < s_pending_input_block_count; ++i) {
        release_pcm_block(s_pending_input_indices[i]);
    }
    s_pending_input_block_count = 0U;
    s_pending_input_pcm_bytes = 0U;
}

static int websocket_send(const char *json, size_t length)
{
    if (s_ws_client == NULL || !s_ws_connected || json == NULL ||
        length == 0U || length > INT_MAX) {
        return -1;
    }
    return esp_websocket_client_send_text(
        s_ws_client, json, (int)length,
        pdMS_TO_TICKS(VOICE_V2_SEND_TIMEOUT_MS));
}

static esp_err_t send_simple_event(const char *type)
{
    size_t length = 0U;
    esp_err_t err = voice_v2_protocol_build_simple_event(
        s_tx_json, VOICE_V2_TX_JSON_BYTES, type, &length);
    if (err != ESP_OK) return err;
    if (websocket_send(s_tx_json, length) != (int)length) return ESP_FAIL;
    strlcpy(s_last_client_event, type, sizeof(s_last_client_event));
    ESP_LOGI(TAG, "CLIENT_EVENT type=%s time=%" PRIu64 "ms", type,
             (uint64_t)esp_timer_get_time() / 1000ULL);
    return ESP_OK;
}

static void cancel_response_if_active(void)
{
    if (!s_response_create_sent || !s_ws_connected || !s_session_ready) {
        return;
    }
    if (send_simple_event("response.cancel") == ESP_OK) {
        s_ignore_cancelled_response = true;
    }
}

static esp_err_t send_session_update(bool include_static_configuration)
{
    char context[VOICE_V2_CONTEXT_BYTES] = {0};
    if (s_context_provider != NULL) {
        esp_err_t context_error = s_context_provider(
            context, sizeof(context), s_context_context);
        if (context_error != ESP_OK) context[0] = '\0';
    }
    char instructions[VOICE_V2_INSTRUCTIONS_BYTES];
    const char *response_style =
        "\nReply directly, naturally and concisely. Use one or two sentences unless the user explicitly asks for detail.";
    if (context[0] != '\0') {
        snprintf(instructions, sizeof(instructions), "%s%s\nDevice context: %s",
                 CONFIG_SMARTSCORE_QWEN_AUDIO_REALTIME_INSTRUCTIONS,
                 response_style, context);
    } else {
        snprintf(instructions, sizeof(instructions), "%s%s",
                 CONFIG_SMARTSCORE_QWEN_AUDIO_REALTIME_INSTRUCTIONS,
                 response_style);
    }

    size_t length = 0U;
    esp_err_t err = voice_v2_protocol_build_session_update(
        s_tx_json, VOICE_V2_TX_JSON_BYTES,
        CONFIG_SMARTSCORE_QWEN_AUDIO_REALTIME_VOICE, instructions,
        include_static_configuration, &length);
    if (err != ESP_OK) return err;
    if (websocket_send(s_tx_json, length) != (int)length) return ESP_FAIL;
    if (include_static_configuration) s_session_update_sent = true;
    strlcpy(s_last_client_event, "session.update",
            sizeof(s_last_client_event));
    ESP_LOGI(TAG,
             "CLIENT_EVENT type=session.update time=%" PRIu64
             "ms mode=%s model=qwen-audio-3.0-realtime-plus"
             " input=pcm/16000/16/mono output=pcm/24000/16/mono voice=%s",
             (uint64_t)esp_timer_get_time() / 1000ULL,
             include_static_configuration ? "initial-push-to-talk"
                                          : "context-refresh",
             CONFIG_SMARTSCORE_QWEN_AUDIO_REALTIME_VOICE);
    return ESP_OK;
}

static esp_err_t send_input_batch(void)
{
    if (s_pending_input_block_count == 0U) {
        while (s_pending_input_block_count < VOICE_V2_SEND_BLOCKS &&
               xQueueReceive(
                   s_pcm_ready_queue,
                   &s_pending_input_indices[s_pending_input_block_count], 0) ==
                   pdTRUE) {
            voice_v2_pcm_block_t *block =
                &s_pcm_blocks[s_pending_input_indices[
                    s_pending_input_block_count]];
            const size_t bytes = block->sample_count * sizeof(int16_t);
            memcpy(s_send_pcm + s_pending_input_pcm_bytes, block->pcm, bytes);
            s_pending_input_pcm_bytes += bytes;
            if (block->speech) ++s_candidate_speech_frames;
            ++s_pending_input_block_count;
        }
    }
    if (s_pending_input_block_count == 0U) return ESP_ERR_NOT_FOUND;

    size_t json_length = 0U;
    esp_err_t err = voice_v2_protocol_build_audio_append(
        s_tx_json, VOICE_V2_TX_JSON_BYTES, s_send_pcm,
        s_pending_input_pcm_bytes,
        &json_length);
    const uint64_t send_start_us = (uint64_t)esp_timer_get_time();
    if (err == ESP_OK) {
        const int sent = websocket_send(s_tx_json, json_length);
        if (sent != (int)json_length) err = ESP_FAIL;
    }
    const uint64_t send_elapsed_us =
        (uint64_t)esp_timer_get_time() - send_start_us;
    if (send_elapsed_us >= VOICE_V2_INPUT_SLOW_WARN_US) {
        ESP_LOGW(TAG,
                 "input WebSocket backpressure wait=%" PRIu64
                 "ms pcm=%u queued=%u result=%s",
                 send_elapsed_us / 1000ULL,
                 (unsigned)s_pending_input_pcm_bytes,
                 (unsigned)uxQueueMessagesWaiting(s_pcm_ready_queue),
                 err == ESP_OK ? "sent" : "deferred");
    }
    if (err == ESP_OK) {
        s_pcm_up_bytes += s_pending_input_pcm_bytes;
        strlcpy(s_last_client_event, "input_audio_buffer.append",
                sizeof(s_last_client_event));
        release_pending_input_batch();
    }
    return err;
}

static void reset_session_fields(void)
{
    s_accept_pcm = false;
    release_pending_input_batch();
    drain_pcm_queue();
    drain_message_queue();
    s_route_confirmed = false;
    s_speech_end_received = false;
    s_input_commit_sent = false;
    s_input_commit_ack = false;
    s_response_create_sent = false;
    s_audio_stream_started = false;
    s_audio_stream_held = false;
    s_audio_done = false;
    s_response_done = false;
    s_finish_sent = false;
    s_speaker_was_active = false;
    s_completion_reported = false;
    s_local_cancel = false;
    s_stall_drain_requested = false;
    s_candidate_failure = false;
    s_candidate_failure_error = ESP_OK;
    s_finish_sent_us = 0U;
    s_last_business_rx_us = 0U;
    s_last_response_business_rx_us = 0U;
    s_last_audio_delta_us = 0U;
    s_audio_flow_start_us = 0U;
    s_next_input_send_us = 0U;
    s_pcm_up_bytes = 0U;
    s_pcm_down_bytes = 0U;
    s_audio_flow_bytes = 0U;
    s_business_events = 0U;
    s_audio_delta_index = 0U;
    s_candidate_speech_frames = 0U;
    s_input_send_retries = 0U;
    s_fast_flush_batches = 0U;
    portENTER_CRITICAL(&s_pcm_pool_lock);
    s_observed_speech_frames = 0U;
    s_pcm_input_bytes = 0U;
    portEXIT_CRITICAL(&s_pcm_pool_lock);
    s_stop_reason[0] = '\0';
    strlcpy(s_last_server_event, "none", sizeof(s_last_server_event));
    strlcpy(s_last_client_event, "none", sizeof(s_last_client_event));
    s_active_audio_gap_us = 0U;
    s_active_audio_delta_index = 0U;
    memset(&s_latency, 0, sizeof(s_latency));
}

static void abort_speaker_stream_safely(void)
{
    if (!s_audio_stream_started) return;
    esp_err_t err = speaker_service_stream_abort();
    if (err == ESP_OK) {
        const TickType_t poll_ticks = pdMS_TO_TICKS(10U);
        const unsigned polls = VOICE_V2_SPEAKER_ABORT_GRACE_MS / 10U;
        for (unsigned i = 0U; i < polls; ++i) {
            speaker_stream_metrics_t metrics;
            speaker_service_stream_get_metrics(&metrics);
            if (!metrics.active) break;
            vTaskDelay(poll_ticks > 0 ? poll_ticks : 1U);
        }
    }
    s_audio_stream_started = false;
    s_audio_stream_held = false;
}

static void complete_session(const char *reason)
{
    char final_reason[VOICE_V2_REASON_BYTES];
    strlcpy(final_reason, reason != NULL ? reason : "DONE",
            sizeof(final_reason));
    const bool notify_owner = !s_completion_reported && !s_local_cancel;
    reset_session_fields();
    set_state(VOICE_V2_STATE_IDLE, final_reason);
    if (notify_owner) {
        report_event(VOICE_ASSISTANT_V2_EVENT_SESSION_ENDED, ESP_OK,
                     final_reason);
    } else {
        ESP_LOGI(TAG, "wake path re-armed");
    }
}

static void fail_session(const char *reason, esp_err_t error)
{
    char final_reason[VOICE_V2_REASON_BYTES];
    strlcpy(final_reason,
            reason != NULL ? reason : "voice-v2-error",
            sizeof(final_reason));
    s_accept_pcm = false;
    if (s_ws_connected && s_session_ready && s_ws_client != NULL &&
        esp_websocket_client_is_connected(s_ws_client)) {
        if (s_response_create_sent) {
            cancel_response_if_active();
        } else if (s_pcm_up_bytes > 0U && !s_input_commit_sent) {
            (void)send_simple_event("input_audio_buffer.clear");
        }
    }
    abort_speaker_stream_safely();
    reset_session_fields();
    set_state(VOICE_V2_STATE_IDLE, final_reason);
    report_event(VOICE_ASSISTANT_V2_EVENT_ERROR,
                 error != ESP_OK ? error : ESP_FAIL, final_reason);
}

static void fail_or_defer_candidate(const char *reason, esp_err_t error)
{
    if (s_state == VOICE_V2_STATE_CANDIDATE && !s_route_confirmed) {
        s_candidate_failure = true;
        s_candidate_failure_error = error != ESP_OK ? error : ESP_FAIL;
        strlcpy(s_stop_reason,
                reason != NULL ? reason : "candidate-cloud-error",
                sizeof(s_stop_reason));
        s_accept_pcm = false;
        release_pending_input_batch();
        drain_pcm_queue();
        ESP_LOGW(TAG,
                 "candidate cloud unavailable (%s); local command recognition remains active",
                 s_stop_reason);
        return;
    }
    fail_session(reason, error);
}

static void begin_closing(const char *reason)
{
    if (s_state == VOICE_V2_STATE_CLOSING) return;
    s_accept_pcm = false;
    strlcpy(s_stop_reason, reason != NULL ? reason : "done",
            sizeof(s_stop_reason));
    /* Qwen's WebSocket session is intentionally kept warm across turns.
     * A drained response closes only the local turn, not the transport. */
    set_state(VOICE_V2_STATE_CLOSING, reason);
    complete_session(reason);
}

static void release_candidate_audio(void)
{
    if (!s_audio_stream_started || !s_audio_stream_held) return;
    esp_err_t err = speaker_service_stream_release();
    if (err == ESP_OK) {
        s_audio_stream_held = false;
        ESP_LOGI(TAG, "AI route confirmed; candidate PCM released");
    } else {
        fail_session("speaker-release-failed", err);
    }
}

static void confirm_ai_route(const char *reason)
{
    if (!s_route_confirmed) {
        s_route_confirmed = true;
        ESP_LOGI(TAG, "AI route confirmed reason=%s", reason);
    }
    if (s_state == VOICE_V2_STATE_CANDIDATE) {
        set_state(VOICE_V2_STATE_LISTENING, reason);
    }
    release_candidate_audio();
}

static void cancel_for_local_command(void)
{
    const bool had_cloud_output =
        s_audio_stream_started || s_state == VOICE_V2_STATE_SPEAKING;
    s_accept_pcm = false;
    s_local_cancel = true;
    release_pending_input_batch();
    drain_pcm_queue();
    drain_message_queue();
    abort_speaker_stream_safely();
    if (s_ws_connected && s_session_ready) {
        if (had_cloud_output || s_response_create_sent) {
            cancel_response_if_active();
        }
        if (s_pcm_up_bytes > 0U && !s_input_commit_sent) {
            (void)send_simple_event("input_audio_buffer.clear");
        }
    }
    ESP_LOGI(TAG, "local command won; candidate cloud output discarded");
    complete_session("local-command");
}

static void handle_owner_command(const voice_v2_command_t *command)
{
    if (command == NULL) return;
    switch (command->type) {
    case VOICE_V2_COMMAND_WAKE:
        if (s_state != VOICE_V2_STATE_IDLE) {
            if (s_ws_connected && s_session_ready) {
                if (s_response_create_sent) {
                    cancel_response_if_active();
                } else if (s_pcm_up_bytes > 0U && !s_input_commit_sent) {
                    (void)send_simple_event("input_audio_buffer.clear");
                }
            }
            abort_speaker_stream_safely();
            reset_session_fields();
            set_state(VOICE_V2_STATE_IDLE, "new-wake-reset");
        } else {
            reset_session_fields();
        }
        s_latency.wake_us = (uint64_t)esp_timer_get_time();
        s_next_summary_us = s_latency.wake_us + VOICE_V2_SUMMARY_INTERVAL_US;
        s_accept_pcm = true;
        set_state(VOICE_V2_STATE_CANDIDATE, "wake");
        ESP_LOGI(TAG, "WAKE: cloud candidate opened; local commands retain priority");
        if (s_ws_connected && s_session_ready && !s_candidate_failure &&
            send_session_update(false) != ESP_OK) {
            fail_or_defer_candidate("session-context-update-failed",
                                    ESP_FAIL);
        }
        break;
    case VOICE_V2_COMMAND_AI_BEGIN:
        if (s_state == VOICE_V2_STATE_IDLE ||
            s_state == VOICE_V2_STATE_CLOSING) break;
        if (s_candidate_failure) {
            fail_session(s_stop_reason,
                         s_candidate_failure_error);
            break;
        }
        mark_once(&s_latency.ai_begin_us);
        confirm_ai_route("AI_BEGIN");
        break;
    case VOICE_V2_COMMAND_SPEECH_END:
        if (s_state == VOICE_V2_STATE_IDLE ||
            s_state == VOICE_V2_STATE_CLOSING) break;
        if (s_candidate_failure) {
            fail_session(s_stop_reason,
                         s_candidate_failure_error);
            break;
        }
        s_accept_pcm = false;
        s_speech_end_received = true;
        /* The S3 boundary is authoritative.  From this point latency matters
         * more than real-time pacing: wake the owner immediately and flush
         * all already-ordered tail PCM before commit. */
        s_next_input_send_us = 0U;
        mark_once(&s_latency.speech_end_us);
        confirm_ai_route("SPEECH_END");
        ESP_LOGI(TAG,
                 "input upload stopped reason=s3-speech-end total_pcm=%" PRIu64
                 " queued_frames=%u pending_frames=%u fast_flush=enabled",
                 pcm_input_total(),
                 (unsigned)uxQueueMessagesWaiting(s_pcm_ready_queue),
                 (unsigned)s_pending_input_block_count);
        break;
    case VOICE_V2_COMMAND_LOCAL_CANCEL:
        if (s_state != VOICE_V2_STATE_IDLE) cancel_for_local_command();
        break;
    case VOICE_V2_COMMAND_NETWORK_READY:
        s_network_ready = command->flag;
        if (!s_network_ready) {
            s_ws_connected = false;
            s_ws_starting = false;
            s_session_update_sent = false;
            s_session_ready = false;
            s_ignore_cancelled_response = false;
            if (s_state != VOICE_V2_STATE_IDLE) {
                fail_session("network-offline", ESP_ERR_INVALID_STATE);
            }
            if (s_ws_client != NULL) {
                (void)esp_websocket_client_stop(s_ws_client);
                (void)esp_websocket_client_destroy(s_ws_client);
                s_ws_client = NULL;
            }
        }
        break;
    case VOICE_V2_COMMAND_WS_CONNECTED:
        s_ws_connected = true;
        s_ws_starting = false;
        report_event(VOICE_ASSISTANT_V2_EVENT_CONNECTED, ESP_OK, NULL);
        ESP_LOGI(TAG, "Qwen Audio Realtime WebSocket connected; awaiting session.created");
        break;
    case VOICE_V2_COMMAND_WS_DISCONNECTED:
    case VOICE_V2_COMMAND_WS_CLOSED:
        s_ws_connected = false;
        s_ws_starting = false;
        s_session_update_sent = false;
        s_session_ready = false;
        s_ignore_cancelled_response = false;
        ESP_LOGW(TAG, "WebSocket disconnected reason=%s",
                 command->reason[0] != '\0' ? command->reason : "unknown");
        if (s_state != VOICE_V2_STATE_IDLE) {
            fail_or_defer_candidate(
                command->reason[0] != '\0'
                    ? command->reason : "websocket-disconnected",
                ESP_ERR_INVALID_STATE);
        }
        break;
    case VOICE_V2_COMMAND_WS_ERROR:
    case VOICE_V2_COMMAND_WS_PROTOCOL_ERROR:
        ESP_LOGE(TAG, "WebSocket error reason=%s error=%s",
                 command->reason[0] != '\0' ? command->reason : "unknown",
                 esp_err_to_name(command->error != ESP_OK
                                     ? command->error : ESP_FAIL));
        if (s_state != VOICE_V2_STATE_IDLE) {
            fail_or_defer_candidate(
                command->reason[0] != '\0'
                    ? command->reason : "websocket-error",
                command->error != ESP_OK ? command->error : ESP_FAIL);
        }
        break;
    default:
        break;
    }
}

static bool prepare_audio_stream(void)
{
    if (s_audio_stream_started) return true;
    esp_err_t err = s_route_confirmed
                        ? speaker_service_stream_start(
                              VOICE_V2_OUTPUT_RATE_HZ)
                        : speaker_service_stream_start_held(
                              VOICE_V2_OUTPUT_RATE_HZ);
    if (err != ESP_OK) {
        fail_session("speaker-stream-start-failed", err);
        return false;
    }
    s_audio_stream_started = true;
    s_audio_stream_held = !s_route_confirmed;
    return true;
}

static void finish_active_message(void)
{
    release_ws_slot(s_active_message);
    s_active_message = -1;
    memset(&s_active_event, 0, sizeof(s_active_event));
    s_audio_encoded_offset = 0U;
    s_audio_decoded_bytes = 0U;
    s_audio_decoded_offset = 0U;
    s_active_delta_pcm_bytes = 0U;
    s_active_audio_rx_us = 0U;
    s_active_audio_gap_us = 0U;
    s_active_audio_delta_index = 0U;
}

static void log_server_event(const voice_v2_server_event_t *event)
{
    if (event == NULL) return;
    if (event->kind == VOICE_V2_SERVER_TEXT_DELTA ||
        event->kind == VOICE_V2_SERVER_AUDIO_DELTA) {
        return;
    }
    ESP_LOGI(TAG, "SERVER_EVENT type=%s time=%" PRIu64 "ms",
             event->type, (uint64_t)esp_timer_get_time() / 1000ULL);
}

static void handle_non_audio_server_event(
    const voice_v2_server_event_t *event)
{
    if (event == NULL) return;
    strlcpy(s_last_server_event, event->type,
            sizeof(s_last_server_event));
    log_server_event(event);

    if (s_state == VOICE_V2_STATE_IDLE &&
        event->kind != VOICE_V2_SERVER_SESSION_CREATED &&
        event->kind != VOICE_V2_SERVER_SESSION_UPDATED &&
        event->kind != VOICE_V2_SERVER_INPUT_CLEARED &&
        event->kind != VOICE_V2_SERVER_ERROR &&
        event->kind != VOICE_V2_SERVER_IGNORED) {
        if (event->kind == VOICE_V2_SERVER_RESPONSE_DONE &&
            (span_equals(&event->response_status, "cancelled") ||
             span_equals(&event->response_status, "canceled"))) {
            s_ignore_cancelled_response = false;
        }
        ESP_LOGI(TAG, "late event ignored while IDLE type=%s", event->type);
        return;
    }

    switch (event->kind) {
    case VOICE_V2_SERVER_SESSION_CREATED:
        mark_once(&s_latency.session_created_us);
        s_session_ready = false;
        if (!s_session_update_sent && send_session_update(true) != ESP_OK) {
            fail_or_defer_candidate("session-update-send-failed", ESP_FAIL);
        }
        break;
    case VOICE_V2_SERVER_SESSION_UPDATED: {
        const bool first_ready = !s_session_ready;
        s_session_ready = true;
        ESP_LOGI(TAG,
                 "QWEN_AUDIO negotiated model=qwen-audio-3.0-realtime-plus codec=pcm_s16le rate=24000 bits=16 channels=1 input_rate=16000");
        if (first_ready) {
            report_event(VOICE_ASSISTANT_V2_EVENT_SESSION_STARTED, ESP_OK,
                         NULL);
        }
        break;
    }
    case VOICE_V2_SERVER_INPUT_COMMITTED:
        s_input_commit_ack = true;
        s_accept_pcm = false;
        report_event(VOICE_ASSISTANT_V2_EVENT_INPUT_COMPLETED, ESP_OK,
                     "input-committed");
        /* This acknowledgement never changes SPEAKING back to WAIT_RESPONSE. */
        break;
    case VOICE_V2_SERVER_INPUT_CLEARED:
        break;
    case VOICE_V2_SERVER_ASR_STARTED:
    case VOICE_V2_SERVER_ASR_DELTA:
        mark_once(&s_latency.first_asr_us);
        break;
    case VOICE_V2_SERVER_ASR_COMPLETED:
        mark_once(&s_latency.asr_done_us);
        s_accept_pcm = false;
        s_speech_end_received = true;
        if (event->transcript.data != NULL) {
            ESP_LOGI(TAG, "ASR completed: %.*s",
                     (int)event->transcript.length,
                     event->transcript.data);
        }
        break;
    case VOICE_V2_SERVER_ASR_FAILED:
        fail_or_defer_candidate("asr-failed", ESP_FAIL);
        break;
    case VOICE_V2_SERVER_TEXT_DONE:
        if (event->transcript.data != NULL) {
            ESP_LOGI(TAG, "assistant: %.*s",
                     (int)event->transcript.length,
                     event->transcript.data);
        } else if (event->text.data != NULL) {
            ESP_LOGI(TAG, "assistant: %.*s", (int)event->text.length,
                     event->text.data);
        }
        break;
    case VOICE_V2_SERVER_AUDIO_STARTED:
        if (s_stall_drain_requested ||
            s_state == VOICE_V2_STATE_DRAINING ||
            s_state == VOICE_V2_STATE_CLOSING ||
            s_state == VOICE_V2_STATE_IDLE) {
            ESP_LOGI(TAG, "late audio-start ignored while draining/closed");
            break;
        }
        mark_once(&s_latency.audio_started_us);
        if (!prepare_audio_stream()) break;
        set_state(VOICE_V2_STATE_SPEAKING, "response.audio.started");
        release_candidate_audio();
        break;
    case VOICE_V2_SERVER_AUDIO_DONE:
        s_audio_done = true;
        mark_once(&s_latency.audio_done_us);
        break;
    case VOICE_V2_SERVER_RESPONSE_DONE:
        mark_once(&s_latency.response_done_us);
        if (event->response_status.data == NULL ||
            span_equals(&event->response_status, "completed")) {
            s_response_done = true;
            if (!s_audio_stream_started) {
                ESP_LOGW(TAG,
                         "response completed without audio PCM; releasing turn safely");
                complete_session("response-complete-without-audio");
            }
        } else if (span_equals(&event->response_status, "cancelled") ||
                   span_equals(&event->response_status, "canceled")) {
            if (s_ignore_cancelled_response) {
                s_ignore_cancelled_response = false;
                ESP_LOGI(TAG, "expected response.done status=cancelled ignored");
            } else if (!s_local_cancel && !s_stall_drain_requested &&
                s_state != VOICE_V2_STATE_DRAINING &&
                s_state != VOICE_V2_STATE_CLOSING &&
                s_state != VOICE_V2_STATE_IDLE) {
                fail_session("server-response-cancelled", ESP_FAIL);
            }
        } else if (span_equals(&event->response_status, "failed")) {
            fail_session("server-response-failed", ESP_FAIL);
        }
        break;
    case VOICE_V2_SERVER_RESPONSE_CANCELED:
        if (s_ignore_cancelled_response || s_local_cancel ||
            s_stall_drain_requested ||
            s_state == VOICE_V2_STATE_DRAINING ||
            s_state == VOICE_V2_STATE_CLOSING ||
            s_state == VOICE_V2_STATE_IDLE) {
            s_ignore_cancelled_response = false;
            ESP_LOGI(TAG, "expected/late response.canceled ignored state=%s",
                     state_name(s_state));
        } else {
            fail_session("server-response-canceled", ESP_FAIL);
        }
        break;
    case VOICE_V2_SERVER_SESSION_CLOSED:
        s_session_update_sent = false;
        s_session_ready = false;
        if (s_state == VOICE_V2_STATE_CLOSING) {
            complete_session(s_stop_reason[0] != '\0'
                                 ? s_stop_reason : "DONE");
        } else if (s_state == VOICE_V2_STATE_DRAINING &&
                   s_stall_drain_requested) {
            /* response.cancel may race a server-side session shutdown.  The
             * cloud is gone, but all PCM already accepted by the speaker must
             * still drain before the local wake path is restored. */
            ESP_LOGW(TAG,
                     "session closed during stalled-response drain; buffered PCM retained");
        } else if (s_state != VOICE_V2_STATE_IDLE) {
            fail_session("unexpected-session-close", ESP_FAIL);
        }
        break;
    case VOICE_V2_SERVER_ERROR:
        if (event->error_message.data != NULL) {
            ESP_LOGE(TAG, "Qwen Audio error code=%.*s message=%.*s",
                     (int)event->error_code.length,
                     event->error_code.data != NULL
                         ? event->error_code.data : "",
                     (int)event->error_message.length,
                     event->error_message.data);
        }
        if (s_state == VOICE_V2_STATE_IDLE) {
            ESP_LOGW(TAG, "idle Qwen Audio error recorded; transport remains warm");
        } else {
            fail_or_defer_candidate("qwen-audio-error", ESP_FAIL);
        }
        break;
    case VOICE_V2_SERVER_UNKNOWN:
        ESP_LOGW(TAG, "unknown Qwen Audio event type=%s", event->type);
        break;
    case VOICE_V2_SERVER_IGNORED:
        break;
    default:
        break;
    }
}

static bool load_next_message(void)
{
    if (s_active_message >= 0) return true;
    uint8_t slot_index = 0U;
    if (xQueueReceive(s_message_queue, &slot_index, 0) != pdTRUE) {
        return false;
    }
    if (slot_index >= VOICE_V2_WS_SLOT_COUNT) return false;
    s_active_message = (int)slot_index;
    voice_v2_ws_slot_t *slot = &s_ws_slots[slot_index];
    esp_err_t err = voice_v2_protocol_parse_server_event(
        (const char *)slot->data, slot->length, &s_active_event);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "invalid Qwen Audio business message bytes=%u",
                 (unsigned)slot->length);
        finish_active_message();
        fail_or_defer_candidate("invalid-server-message", err);
        return false;
    }
    ++s_business_events;
    s_last_business_rx_us = slot->completed_us;
    if (is_response_event(s_active_event.type)) {
        s_last_response_business_rx_us = slot->completed_us;
    }
    strlcpy(s_last_server_event, s_active_event.type,
            sizeof(s_last_server_event));
    if (s_active_event.kind != VOICE_V2_SERVER_AUDIO_DELTA) {
        handle_non_audio_server_event(&s_active_event);
        finish_active_message();
        return false;
    }
    if (s_stall_drain_requested ||
        s_state == VOICE_V2_STATE_DRAINING ||
        s_state == VOICE_V2_STATE_CLOSING ||
        s_state == VOICE_V2_STATE_IDLE) {
        /* The owner has canceled an upstream response after a real business
         * stall.  Do not let a late delta reopen SPEAKING after finish was
         * queued; the PCM received before cancellation remains in the ring. */
        finish_active_message();
        return false;
    }
    if (s_active_event.delta.data == NULL ||
        s_active_event.delta.length == 0U ||
        (s_active_event.delta.length & 3U) != 0U) {
        finish_active_message();
        fail_session("invalid-audio-delta", ESP_ERR_INVALID_RESPONSE);
        return false;
    }
    s_active_audio_rx_us = slot->completed_us;
    s_active_audio_gap_us =
        s_last_audio_delta_us > 0U &&
                s_active_audio_rx_us > s_last_audio_delta_us
            ? s_active_audio_rx_us - s_last_audio_delta_us
            : 0U;
    s_last_audio_delta_us = s_active_audio_rx_us;
    s_active_audio_delta_index = ++s_audio_delta_index;
    if (s_audio_flow_start_us == 0U) {
        s_audio_flow_start_us = s_active_audio_rx_us;
    }
    mark_once(&s_latency.audio_started_us);
    mark_once(&s_latency.first_pcm_us);
    if (!prepare_audio_stream()) {
        finish_active_message();
        return false;
    }
    set_state(VOICE_V2_STATE_SPEAKING, "response.audio.delta");
    release_candidate_audio();
    return true;
}

static bool pump_active_audio(void)
{
    if (s_active_message < 0 ||
        s_active_event.kind != VOICE_V2_SERVER_AUDIO_DELTA) {
        return false;
    }
    speaker_stream_metrics_t metrics;
    speaker_service_stream_get_metrics(&metrics);
    if (!metrics.active) return false;

    if (s_audio_decoded_offset < s_audio_decoded_bytes) {
        const size_t remaining =
            s_audio_decoded_bytes - s_audio_decoded_offset;
        size_t accepted_samples = 0U;
        esp_err_t err = speaker_service_stream_write(
            (const int16_t *)(s_decode_pcm + s_audio_decoded_offset),
            remaining / sizeof(int16_t), &accepted_samples);
        const size_t accepted_bytes = accepted_samples * sizeof(int16_t);
        s_audio_decoded_offset += accepted_bytes;
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            finish_active_message();
            fail_session("speaker-stream-write-failed", err);
            return false;
        }
        if (s_audio_decoded_offset < s_audio_decoded_bytes) return false;
        s_audio_decoded_offset = 0U;
        s_audio_decoded_bytes = 0U;
    }

    if (s_audio_encoded_offset < s_active_event.delta.length) {
        size_t encoded_bytes =
            s_active_event.delta.length - s_audio_encoded_offset;
        const size_t max_encoded = (VOICE_V2_DECODE_BYTES / 3U) * 4U;
        if (encoded_bytes > max_encoded) encoded_bytes = max_encoded;
        if (encoded_bytes <
            s_active_event.delta.length - s_audio_encoded_offset) {
            encoded_bytes &= ~(size_t)3U;
        }
        voice_v2_json_span_t span = {
            .data = s_active_event.delta.data + s_audio_encoded_offset,
            .length = encoded_bytes,
        };
        size_t decoded = 0U;
        esp_err_t err = voice_v2_protocol_decode_audio_span(
            &span, s_decode_pcm, VOICE_V2_DECODE_BYTES, &decoded);
        if (err != ESP_OK) {
            finish_active_message();
            fail_session("audio-delta-base64-invalid", err);
            return false;
        }
        s_audio_encoded_offset += encoded_bytes;
        s_audio_decoded_bytes = decoded;
        s_audio_decoded_offset = 0U;
        s_pcm_down_bytes += decoded;
        s_audio_flow_bytes += decoded;
        s_active_delta_pcm_bytes += decoded;
        return true;
    }

    const uint64_t flow_elapsed_us =
        s_active_audio_rx_us > s_audio_flow_start_us
            ? s_active_audio_rx_us - s_audio_flow_start_us : 0U;
    const uint64_t average_bytes_per_second = flow_elapsed_us > 0U
        ? s_audio_flow_bytes * 1000000ULL / flow_elapsed_us : 0U;
    ESP_LOGI(TAG,
             "AUDIO_FLOW idx=%" PRIu32 " bytes=%u gap=%" PRIu64
              "ms total=%" PRIu64 " avg=%" PRIu64 "B/s",
              s_active_audio_delta_index,
              (unsigned)s_active_delta_pcm_bytes,
              s_active_audio_gap_us / 1000ULL, s_audio_flow_bytes,
              average_bytes_per_second);
    if (s_active_audio_gap_us > VOICE_V2_AUDIO_GAP_WARN_US ||
        (flow_elapsed_us >= 1000000ULL &&
         average_bytes_per_second < 48000ULL)) {
        ESP_LOGW(TAG,
                 "UPSTREAM_AUDIO_GAP gap=%" PRIu64
                  "ms avg=%" PRIu64 "B/s expected=48000B/s total=%" PRIu64,
                  s_active_audio_gap_us / 1000ULL,
                  average_bytes_per_second,
                  s_audio_flow_bytes);
    }
    finish_active_message();
    return true;
}

static void process_server_messages(void)
{
    for (unsigned budget = 0U;
         budget < VOICE_V2_SERVER_WORK_BUDGET;
         ++budget) {
        if (s_active_message < 0 &&
            uxQueueMessagesWaiting(s_message_queue) == 0U) {
            break;
        }
        if (!load_next_message()) continue;
        if (s_active_event.kind == VOICE_V2_SERVER_AUDIO_DELTA) {
            if (!pump_active_audio()) break;
            /* Keep pumping the same complete audio message while the speaker
             * ring accepts data.  The write path is non-blocking, so this
             * releases its large WebSocket slot promptly without delaying RX. */
            continue;
        }
    }
}

static void process_input(void)
{
    if (!s_session_ready || !s_ws_connected ||
        s_candidate_failure ||
        s_state == VOICE_V2_STATE_IDLE ||
        s_state == VOICE_V2_STATE_CLOSING) {
        return;
    }
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    if ((s_pending_input_block_count > 0U ||
         uxQueueMessagesWaiting(s_pcm_ready_queue) > 0U) &&
        now_us >= s_next_input_send_us) {
        esp_err_t err = send_input_batch();
        if (err == ESP_OK) {
            s_input_send_retries = 0U;
            const UBaseType_t queued_after =
                uxQueueMessagesWaiting(s_pcm_ready_queue);
            const bool more_pcm =
                s_pending_input_block_count > 0U ||
                queued_after > 0U;
            const bool fast_flush =
                more_pcm &&
                (s_speech_end_received ||
                 queued_after >= VOICE_V2_FAST_FLUSH_QUEUE_BLOCKS);
            if (fast_flush) {
                ++s_fast_flush_batches;
                s_next_input_send_us = 0U;
                /* Avoid the normal 10 ms task wait between history batches;
                 * websocket_send remains bounded and the RX callback remains
                 * on its independent task. */
                if (s_task != NULL) xTaskNotifyGive(s_task);
            } else {
                s_next_input_send_us =
                    (uint64_t)esp_timer_get_time() +
                    VOICE_V2_INPUT_SEND_GAP_US;
            }
        } else if (err != ESP_ERR_NOT_FOUND && s_ws_client != NULL &&
                   esp_websocket_client_is_connected(s_ws_client) &&
                   s_input_send_retries < VOICE_V2_INPUT_SEND_RETRIES) {
            ++s_input_send_retries;
            s_next_input_send_us =
                (uint64_t)esp_timer_get_time() + VOICE_V2_INPUT_RETRY_GAP_US;
            ESP_LOGW(TAG,
                     "input send deferred retry=%u/%u pending_pcm=%u queued=%u",
                     (unsigned)s_input_send_retries,
                     (unsigned)VOICE_V2_INPUT_SEND_RETRIES,
                     (unsigned)s_pending_input_pcm_bytes,
                     (unsigned)uxQueueMessagesWaiting(s_pcm_ready_queue));
        } else if (err != ESP_ERR_NOT_FOUND) {
            fail_session("input-audio-send-failed", err);
            return;
        }
    }
    if (s_speech_end_received && !s_input_commit_sent &&
        !s_input_commit_ack &&
        s_pending_input_block_count == 0U &&
        uxQueueMessagesWaiting(s_pcm_ready_queue) == 0U) {
        esp_err_t err = send_simple_event("input_audio_buffer.commit");
        if (err != ESP_OK) {
            fail_session("input-commit-send-failed", err);
            return;
        }
        s_input_commit_sent = true;
        mark_once(&s_latency.input_commit_us);
        err = send_simple_event("response.create");
        if (err != ESP_OK) {
            fail_session("response-create-send-failed", err);
            return;
        }
        s_response_create_sent = true;
        if (s_state == VOICE_V2_STATE_CANDIDATE ||
            s_state == VOICE_V2_STATE_LISTENING) {
            set_state(VOICE_V2_STATE_WAIT_RESPONSE,
                      "input.commit+response.create");
        }
        ESP_LOGI(TAG,
                 "input committed and response requested exactly once pcm_up=%" PRIu64
                 " fast_flush_batches=%" PRIu32,
                 s_pcm_up_bytes, s_fast_flush_batches);
    }
}

static void process_candidate_timeout(void)
{
    if (s_state != VOICE_V2_STATE_CANDIDATE || s_route_confirmed ||
        s_latency.wake_us == 0U) {
        return;
    }
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    if (now_us - s_latency.wake_us >=
            VOICE_V2_CANDIDATE_TIMEOUT_MS * 1000ULL &&
        s_observed_speech_frames >= VOICE_V2_MIN_SPEECH_FRAMES) {
        if (s_candidate_failure) {
            return;
        }
        confirm_ai_route("candidate-timeout-valid-speech");
    }
}

static void process_playback(void)
{
    if (!s_audio_stream_started) return;
    speaker_stream_metrics_t metrics;
    speaker_service_stream_get_metrics(&metrics);
    if (metrics.active) s_speaker_was_active = true;
    if (metrics.output_started) mark_once(&s_latency.speaker_started_us);

    /* Both official completion events mean no more PCM will be produced.
     * stream_finish never discards queued PCM; it only lets the speaker drain
     * a final buffer smaller than the jitter/rebuffer threshold. */
    const bool producer_done =
        s_audio_done || s_response_done || s_stall_drain_requested;
    if (producer_done && s_active_message < 0 &&
        uxQueueMessagesWaiting(s_message_queue) == 0U && !s_finish_sent) {
        esp_err_t err = speaker_service_stream_finish();
        if (err != ESP_OK) {
            fail_session("speaker-finish-failed", err);
            return;
        }
        s_finish_sent = true;
        s_finish_sent_us = (uint64_t)esp_timer_get_time();
        set_state(VOICE_V2_STATE_DRAINING,
                  s_audio_done
                      ? "response.audio.done"
                      : (s_stall_drain_requested
                             ? "speaking-stalled-drain"
                             : "response.done"));
    }
    if (s_state == VOICE_V2_STATE_DRAINING && s_finish_sent &&
        !metrics.active &&
        (s_speaker_was_active ||
         (uint64_t)esp_timer_get_time() - s_finish_sent_us > 100000ULL)) {
        mark_once(&s_latency.playback_done_us);
        log_latency();
        ESP_LOGI(TAG,
                 "session audio summary input=%" PRIu64
                 " pcm_up=%" PRIu64
                 " pcm_down=%" PRIu64 " played=%" PRIu64
                 " max_buffered=%u underrun=%" PRIu32,
                 pcm_input_total(), s_pcm_up_bytes, s_pcm_down_bytes,
                 metrics.played_bytes,
                 (unsigned)metrics.max_buffered_bytes, metrics.underruns);
        s_audio_stream_started = false;
        begin_closing(s_stall_drain_requested
                          ? "speaking-stalled-drained"
                          : "single-turn-complete");
    }
}

static void process_speaking_stall(void)
{
    if (s_state != VOICE_V2_STATE_SPEAKING ||
        s_audio_done || s_response_done ||
        s_stall_drain_requested ||
        s_last_response_business_rx_us == 0U) {
        return;
    }
    speaker_stream_metrics_t speaker_metrics;
    speaker_service_stream_get_metrics(&speaker_metrics);
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    if (now_us < s_last_response_business_rx_us ||
        now_us - s_last_response_business_rx_us <
            VOICE_V2_SPEAKING_STALL_US) {
        return;
    }

    voice_v2_ws_metrics_t ws_metrics;
    get_ws_metrics(&ws_metrics);
    ESP_LOGE(TAG,
             "VOICE_V2_RECOVERY: reason=speaking-stalled"
             " last_ws_rx_age=%lld last_business_age=%lld"
             " last_audio_delta_age=%lld pcm_down=%" PRIu64
             " played=%" PRIu64 " buffered=%u action=drain-buffered-pcm",
             age_ms(now_us, ws_metrics.last_rx_us),
             age_ms(now_us, s_last_business_rx_us),
             age_ms(now_us, s_last_audio_delta_us),
             s_pcm_down_bytes, speaker_metrics.played_bytes,
             (unsigned)speaker_metrics.buffered_bytes);

    const bool can_send =
        s_ws_connected && s_session_ready && s_ws_client != NULL &&
        esp_websocket_client_is_connected(s_ws_client);
    if (can_send) cancel_response_if_active();
    /* Do not abort or close yet. process_playback() marks the speaker
     * producer finished, releases a below-threshold jitter buffer, drains it
     * completely, and only then closes the cloud session and returns IDLE. */
    s_accept_pcm = false;
    s_stall_drain_requested = true;
}

static void log_summary_if_due(void)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    if (s_state == VOICE_V2_STATE_IDLE || now_us < s_next_summary_us) return;
    s_next_summary_us = now_us + VOICE_V2_SUMMARY_INTERVAL_US;
    speaker_stream_metrics_t metrics;
    speaker_service_stream_get_metrics(&metrics);
    voice_v2_ws_metrics_t ws_metrics;
    get_ws_metrics(&ws_metrics);
    uint32_t ws_slots_in_use = 0U;
    uint32_t ws_slots_max_in_use = 0U;
    uint32_t ws_slot_exhaustions = 0U;
    get_ws_slot_metrics(&ws_slots_in_use, &ws_slots_max_in_use,
                        &ws_slot_exhaustions);
    const bool ws_connected =
        s_ws_connected && s_ws_client != NULL &&
        esp_websocket_client_is_connected(s_ws_client);
    ESP_LOGI(TAG,
             "VOICE_V2 state=%s ws_connected=%s"
             " ws_rx_events=%" PRIu64 " ws_rx_bytes=%" PRIu64
             " last_ws_rx_age_ms=%lld"
             " business_events=%" PRIu64 " last_business_event=%s"
             " last_business_age_ms=%lld"
             " audio_delta_count=%" PRIu32
             " last_audio_delta_age_ms=%lld ws_callback_max_us=%" PRIu64
             " ws_slots=%" PRIu32 "/%u ws_slots_max=%" PRIu32
             " ws_slot_exhaustions=%" PRIu32
             " pcm_up=%" PRIu64
             " pcm_down=%" PRIu64 " speaker_buffered=%u played=%" PRIu64
             " underrun=%" PRIu32,
             state_name(s_state), ws_connected ? "true" : "false",
             ws_metrics.rx_events, ws_metrics.rx_bytes,
             age_ms(now_us, ws_metrics.last_rx_us),
             s_business_events, s_last_server_event,
             age_ms(now_us, s_last_business_rx_us),
             s_audio_delta_index,
             age_ms(now_us, s_last_audio_delta_us),
             ws_metrics.callback_max_us,
             ws_slots_in_use, (unsigned)VOICE_V2_WS_SLOT_COUNT,
             ws_slots_max_in_use, ws_slot_exhaustions,
             s_pcm_up_bytes, s_pcm_down_bytes,
             (unsigned)metrics.buffered_bytes, metrics.played_bytes,
             metrics.underruns);
}

static void websocket_data_event(const esp_websocket_event_data_t *data)
{
    if (data == NULL || data->op_code >= 0x08U) return;
    if (data->data_len < 0 || data->payload_len < 0 ||
        data->payload_offset < 0 ||
        (data->data_len > 0 && data->data_ptr == NULL)) {
        reset_ws_assembler();
        (void)queue_command(VOICE_V2_COMMAND_WS_PROTOCOL_ERROR, false,
                            ESP_ERR_INVALID_RESPONSE,
                            "invalid-websocket-fragment");
        return;
    }

    const size_t chunk = (size_t)data->data_len;
    const size_t offset = (size_t)data->payload_offset;
    const size_t frame_length = data->payload_len > 0
                                    ? (size_t)data->payload_len : chunk;
    if (offset == 0U) {
        if (data->op_code == 0x01U || data->op_code == 0x02U) {
            if (s_ws_assembler.active) {
                reset_ws_assembler();
                (void)queue_command(VOICE_V2_COMMAND_WS_PROTOCOL_ERROR,
                                    false, ESP_ERR_INVALID_RESPONSE,
                                    "websocket-message-overlap");
                return;
            }
            const int slot = allocate_ws_slot();
            if (slot < 0) {
                (void)queue_command(VOICE_V2_COMMAND_WS_PROTOCOL_ERROR,
                                    false, ESP_ERR_NO_MEM,
                                    "websocket-message-pool-full");
                return;
            }
            s_ws_assembler.active = true;
            s_ws_assembler.slot = slot;
            s_ws_assembler.opcode = data->op_code;
        } else if (data->op_code != 0x00U || !s_ws_assembler.active) {
            reset_ws_assembler();
            (void)queue_command(VOICE_V2_COMMAND_WS_PROTOCOL_ERROR, false,
                                ESP_ERR_INVALID_RESPONSE,
                                "websocket-continuation-invalid");
            return;
        }
        s_ws_assembler.frame_expected = frame_length;
        s_ws_assembler.frame_received = 0U;
    } else if (!s_ws_assembler.active) {
        (void)queue_command(VOICE_V2_COMMAND_WS_PROTOCOL_ERROR, false,
                            ESP_ERR_INVALID_RESPONSE,
                            "websocket-tail-without-head");
        return;
    }

    if (offset != s_ws_assembler.frame_received ||
        offset > frame_length || chunk > frame_length - offset) {
        reset_ws_assembler();
        (void)queue_command(VOICE_V2_COMMAND_WS_PROTOCOL_ERROR, false,
                            ESP_ERR_INVALID_RESPONSE,
                            "websocket-fragment-order");
        return;
    }
    voice_v2_ws_slot_t *slot = &s_ws_slots[s_ws_assembler.slot];
    if (chunk > VOICE_V2_WS_SLOT_BYTES ||
        slot->length > VOICE_V2_WS_SLOT_BYTES - chunk) {
        reset_ws_assembler();
        (void)queue_command(VOICE_V2_COMMAND_WS_PROTOCOL_ERROR, false,
                            ESP_ERR_INVALID_SIZE,
                            "websocket-message-too-large");
        return;
    }
    if (chunk > 0U) {
        memcpy(slot->data + slot->length, data->data_ptr, chunk);
        slot->length += chunk;
        s_ws_assembler.frame_received += chunk;
    }
    if (s_ws_assembler.frame_received != s_ws_assembler.frame_expected) {
        return;
    }
    s_ws_assembler.frame_expected = 0U;
    s_ws_assembler.frame_received = 0U;
    if (!data->fin) return;

    slot->data[slot->length] = '\0';
    slot->opcode = s_ws_assembler.opcode;
    slot->completed_us = (uint64_t)esp_timer_get_time();
    uint8_t completed_slot = (uint8_t)s_ws_assembler.slot;
    memset(&s_ws_assembler, 0, sizeof(s_ws_assembler));
    s_ws_assembler.slot = -1;
    if (xQueueSend(s_message_queue, &completed_slot, 0) != pdTRUE) {
        release_ws_slot((int)completed_slot);
        (void)queue_command(VOICE_V2_COMMAND_WS_PROTOCOL_ERROR, false,
                            ESP_ERR_NO_MEM,
                            "websocket-message-queue-full");
        return;
    }
    if (s_task != NULL) xTaskNotifyGive(s_task);
}

static void websocket_event_handler(void *handler_arg,
                                    esp_event_base_t base,
                                    int32_t event_id,
                                    void *event_data)
{
    (void)handler_arg;
    (void)base;
    esp_websocket_event_data_t *data =
        (esp_websocket_event_data_t *)event_data;
    switch ((esp_websocket_event_id_t)event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        reset_ws_assembler();
        (void)queue_command(VOICE_V2_COMMAND_WS_CONNECTED, false,
                            ESP_OK, NULL);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        reset_ws_assembler();
        (void)queue_command(VOICE_V2_COMMAND_WS_DISCONNECTED, false,
                            ESP_ERR_INVALID_STATE,
                            "websocket-disconnected");
        break;
    case WEBSOCKET_EVENT_ERROR: {
        char reason[VOICE_V2_REASON_BYTES] = "websocket-error";
        esp_err_t error = ESP_FAIL;
        if (data != NULL) {
            if (data->error_handle.esp_ws_handshake_status_code >= 400) {
                snprintf(reason, sizeof(reason), "websocket-http-%d",
                         data->error_handle.esp_ws_handshake_status_code);
            }
            if (data->error_handle.esp_tls_last_esp_err != 0) {
                error = data->error_handle.esp_tls_last_esp_err;
            }
        }
        (void)queue_command(VOICE_V2_COMMAND_WS_ERROR, false, error,
                            reason);
        break;
    }
    case WEBSOCKET_EVENT_DATA:
        {
        const uint64_t callback_start_us =
            (uint64_t)esp_timer_get_time();
        portENTER_CRITICAL(&s_ws_metrics_lock);
        ++s_ws_rx_events;
        if (data != NULL && data->data_len > 0) {
            s_ws_rx_bytes += (uint64_t)data->data_len;
        }
        s_last_ws_rx_us = callback_start_us;
        portEXIT_CRITICAL(&s_ws_metrics_lock);
        websocket_data_event(data);
        const uint64_t callback_elapsed_us =
            (uint64_t)esp_timer_get_time() - callback_start_us;
        portENTER_CRITICAL(&s_ws_metrics_lock);
        if (callback_elapsed_us > s_ws_callback_max_us) {
            s_ws_callback_max_us = callback_elapsed_us;
        }
        portEXIT_CRITICAL(&s_ws_metrics_lock);
        if (callback_elapsed_us > VOICE_V2_CALLBACK_WARN_US) {
            ESP_LOGW(TAG, "slow websocket callback %" PRIu64 " us",
                     callback_elapsed_us);
        }
        }
        break;
    case WEBSOCKET_EVENT_CLOSED:
        reset_ws_assembler();
        (void)queue_command(VOICE_V2_COMMAND_WS_CLOSED, false,
                            ESP_ERR_INVALID_STATE,
                            "websocket-peer-closed");
        break;
    default:
        break;
    }
}

static esp_err_t start_websocket(void)
{
    if (s_ws_client != NULL || s_ws_starting || !s_network_ready) {
        return ESP_OK;
    }
    int header_length = snprintf(s_ws_headers, sizeof(s_ws_headers),
                                 "Authorization: Bearer %s\r\n",
                                 QWEN_AUDIO_API_KEY);
    if (header_length <= 0 || header_length >= (int)sizeof(s_ws_headers)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_websocket_client_config_t config = {
        .uri = CONFIG_SMARTSCORE_QWEN_AUDIO_REALTIME_ENDPOINT,
        .disable_auto_reconnect = false,
        .enable_close_reconnect = false,
        .task_prio = VOICE_V2_WS_TASK_PRIORITY,
        .task_name = "qwen_audio_ws",
        .task_stack = VOICE_V2_WS_TASK_STACK_BYTES,
        .buffer_size = VOICE_V2_WS_BUFFER_BYTES,
        .headers = s_ws_headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .ping_interval_sec = 10,
        .reconnect_timeout_ms =
            CONFIG_SMARTSCORE_QWEN_AUDIO_REALTIME_RECONNECT_MS,
        .network_timeout_ms = 10000,
    };
    s_ws_client = esp_websocket_client_init(&config);
    if (s_ws_client == NULL) return ESP_ERR_NO_MEM;
    s_session_update_sent = false;
    s_session_ready = false;
    s_ignore_cancelled_response = false;
    esp_err_t err = esp_websocket_register_events(
        s_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    if (err == ESP_OK) err = esp_websocket_client_start(s_ws_client);
    if (err != ESP_OK) {
        (void)esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
        return err;
    }
    s_ws_starting = true;
    ESP_LOGI(TAG, "connecting warm Qwen Audio WebSocket: %s",
             CONFIG_SMARTSCORE_QWEN_AUDIO_REALTIME_ENDPOINT);
    return ESP_OK;
}

static void voice_v2_task(void *argument)
{
    (void)argument;
    while (true) {
        voice_v2_command_t command;
        while (xQueueReceive(s_command_queue, &command, 0) == pdTRUE) {
            handle_owner_command(&command);
        }
        if (s_network_ready && s_ws_client == NULL) {
            esp_err_t err = start_websocket();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "warm WebSocket start failed: %s",
                         esp_err_to_name(err));
            }
        }
        process_server_messages();
        process_input();
        process_candidate_timeout();
        process_playback();
        process_speaking_stall();
        log_summary_if_due();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(VOICE_V2_TASK_TICK_MS));
    }
}

static void cleanup_allocations(void)
{
    if (s_command_queue != NULL) vQueueDelete(s_command_queue);
    if (s_pcm_ready_queue != NULL) vQueueDelete(s_pcm_ready_queue);
    if (s_message_queue != NULL) vQueueDelete(s_message_queue);
    s_command_queue = NULL;
    s_pcm_ready_queue = NULL;
    s_message_queue = NULL;
    for (unsigned i = 0U; i < VOICE_V2_WS_SLOT_COUNT; ++i) {
        free(s_ws_slots[i].data);
        memset(&s_ws_slots[i], 0, sizeof(s_ws_slots[i]));
    }
    free(s_pcm_blocks);
    free(s_send_pcm);
    free(s_tx_json);
    free(s_decode_pcm);
    free(s_task_stack);
    s_pcm_blocks = NULL;
    s_send_pcm = NULL;
    s_tx_json = NULL;
    s_decode_pcm = NULL;
    s_task_stack = NULL;
}

esp_err_t voice_assistant_v2_init(
    voice_assistant_v2_event_handler_t handler,
    void *context)
{
#if !CONFIG_SMARTSCORE_QWEN_AUDIO_REALTIME_ASSISTANT
    (void)handler;
    (void)context;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_initialized) {
        s_event_handler = handler;
        s_event_context = context;
        return ESP_OK;
    }
    if (QWEN_AUDIO_API_KEY[0] == '\0') {
        ESP_LOGE(TAG,
                 "QWEN_AUDIO_API_KEY is not configured (set DASHSCOPE_API_KEY before reconfigure)");
        return ESP_ERR_INVALID_STATE;
    }
    speaker_status_t speaker;
    speaker_service_get_status(&speaker);
    if (!speaker.hardware_output_enabled) return ESP_ERR_NOT_SUPPORTED;

    s_command_queue = xQueueCreate(VOICE_V2_COMMAND_QUEUE_LENGTH,
                                   sizeof(voice_v2_command_t));
    s_pcm_ready_queue = xQueueCreate(VOICE_V2_PCM_BLOCK_COUNT,
                                     sizeof(uint16_t));
    s_message_queue = xQueueCreate(VOICE_V2_MESSAGE_QUEUE_LENGTH,
                                   sizeof(uint8_t));
    s_pcm_blocks = heap_caps_calloc(
        VOICE_V2_PCM_BLOCK_COUNT, sizeof(voice_v2_pcm_block_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_send_pcm = heap_caps_malloc(
        VOICE_V2_SEND_PCM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_tx_json = heap_caps_malloc(
        VOICE_V2_TX_JSON_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_decode_pcm = heap_caps_malloc(
        VOICE_V2_DECODE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_task_stack = heap_caps_calloc(
        1U, VOICE_V2_TASK_STACK_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (unsigned i = 0U; i < VOICE_V2_WS_SLOT_COUNT; ++i) {
        s_ws_slots[i].data = heap_caps_malloc(
            VOICE_V2_WS_SLOT_BYTES + 1U,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_command_queue == NULL || s_pcm_ready_queue == NULL ||
        s_message_queue == NULL || s_pcm_blocks == NULL ||
        s_send_pcm == NULL || s_tx_json == NULL || s_decode_pcm == NULL ||
        s_task_stack == NULL) {
        cleanup_allocations();
        return ESP_ERR_NO_MEM;
    }
    for (unsigned i = 0U; i < VOICE_V2_WS_SLOT_COUNT; ++i) {
        if (s_ws_slots[i].data == NULL) {
            cleanup_allocations();
            return ESP_ERR_NO_MEM;
        }
    }
    portENTER_CRITICAL(&s_pcm_pool_lock);
    s_pcm_free_count = VOICE_V2_PCM_BLOCK_COUNT;
    for (unsigned i = 0U; i < VOICE_V2_PCM_BLOCK_COUNT; ++i) {
        s_pcm_free_stack[i] = (uint16_t)i;
    }
    portEXIT_CRITICAL(&s_pcm_pool_lock);
    reset_session_fields();
    s_event_handler = handler;
    s_event_context = context;
    s_task = xTaskCreateStatic(
        voice_v2_task, "voice_v2_task", VOICE_V2_TASK_STACK_BYTES,
        NULL, VOICE_V2_TASK_PRIORITY, s_task_stack, &s_task_control);
    if (s_task == NULL) {
        cleanup_allocations();
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG,
             "Qwen Audio Realtime initialized model=qwen-audio-3.0-realtime-plus owner=voice_v2_task input_pool=%u ws_pool=%u speaker_ring=reused-256KiB API-key=configured",
             (unsigned)(VOICE_V2_PCM_BLOCK_COUNT *
                        sizeof(voice_v2_pcm_block_t)),
             (unsigned)(VOICE_V2_WS_SLOT_COUNT * VOICE_V2_WS_SLOT_BYTES));
    return ESP_OK;
#endif
}

void voice_assistant_v2_set_context_provider(
    voice_assistant_v2_context_provider_t provider,
    void *context)
{
    s_context_provider = provider;
    s_context_context = context;
}

void voice_assistant_v2_set_network_ready(bool ready)
{
    if (!s_initialized) return;
    (void)queue_command(VOICE_V2_COMMAND_NETWORK_READY, ready, ESP_OK, NULL);
}

esp_err_t voice_assistant_v2_arm(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return queue_command(VOICE_V2_COMMAND_WAKE, false, ESP_OK, NULL)
               ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t voice_assistant_v2_begin(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return queue_command(VOICE_V2_COMMAND_AI_BEGIN, false, ESP_OK, NULL)
               ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t voice_assistant_v2_speech_end(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return queue_command(VOICE_V2_COMMAND_SPEECH_END, false, ESP_OK, NULL)
               ? ESP_OK : ESP_ERR_TIMEOUT;
}

void voice_assistant_v2_cancel_candidate(void)
{
    if (!s_initialized) return;
    (void)queue_command(VOICE_V2_COMMAND_LOCAL_CANCEL, false, ESP_OK, NULL);
}

esp_err_t voice_assistant_v2_push_audio(const int16_t *pcm,
                                        size_t sample_count,
                                        uint16_t sequence)
{
    if (!s_initialized || pcm == NULL || sample_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_accept_pcm) return ESP_ERR_INVALID_STATE;
    if (sample_count > VOICE_V2_PCM_BLOCK_SAMPLES) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint16_t index = 0U;
    if (!allocate_pcm_block(&index)) {
        ++s_pcm_pool_exhaustions;
        if (s_pcm_pool_exhaustions == 1U ||
            (s_pcm_pool_exhaustions % 50U) == 0U) {
            ESP_LOGW(TAG, "input PSRAM pool backpressure=%" PRIu32,
                     s_pcm_pool_exhaustions);
        }
        return ESP_ERR_TIMEOUT;
    }
    voice_v2_pcm_block_t *block = &s_pcm_blocks[index];
    block->sequence = sequence;
    block->sample_count = (uint16_t)sample_count;
    uint64_t absolute_sum = 0U;
    for (size_t i = 0U; i < sample_count; ++i) {
        const int32_t value = pcm[i];
        absolute_sum += (uint32_t)(value < 0 ? -value : value);
    }
    block->speech = absolute_sum / sample_count >= VOICE_V2_SPEECH_LEVEL;
    const bool speech = block->speech;
    memcpy(block->pcm, pcm, sample_count * sizeof(*pcm));
    if (!s_accept_pcm ||
        xQueueSend(s_pcm_ready_queue, &index, 0) != pdTRUE) {
        release_pcm_block(index);
        return s_accept_pcm ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_pcm_pool_lock);
    s_pcm_input_bytes += sample_count * sizeof(*pcm);
    if (speech) {
        ++s_observed_speech_frames;
    }
    portEXIT_CRITICAL(&s_pcm_pool_lock);
    if (s_task != NULL) xTaskNotifyGive(s_task);
    return ESP_OK;
}
