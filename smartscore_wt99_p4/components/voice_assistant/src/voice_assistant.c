#include "voice_assistant.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"
#include "speaker_service.h"

#ifndef CONFIG_SMARTSCORE_DOUBAO_VOICE_ASSISTANT
#define CONFIG_SMARTSCORE_DOUBAO_VOICE_ASSISTANT 0
#endif
#ifndef CONFIG_SMARTSCORE_DOUBAO_VOICE_ENDPOINT
#define CONFIG_SMARTSCORE_DOUBAO_VOICE_ENDPOINT \
    "wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue"
#endif
#ifndef CONFIG_SMARTSCORE_DOUBAO_VOICE_MODEL
#define CONFIG_SMARTSCORE_DOUBAO_VOICE_MODEL "1.2.6.1"
#endif
#ifndef CONFIG_SMARTSCORE_DOUBAO_VOICE_NAME
#define CONFIG_SMARTSCORE_DOUBAO_VOICE_NAME \
    "zh_female_xiaohe_jupiter_bigtts"
#endif
#ifndef CONFIG_SMARTSCORE_DOUBAO_VOICE_INSTRUCTIONS
#define CONFIG_SMARTSCORE_DOUBAO_VOICE_INSTRUCTIONS \
    "You are the SmartScore piano voice assistant. Answer concisely in Chinese."
#endif
#ifndef CONFIG_SMARTSCORE_DOUBAO_VOICE_INPUT_BUFFER_SECONDS
#define CONFIG_SMARTSCORE_DOUBAO_VOICE_INPUT_BUFFER_SECONDS 8
#endif
#ifndef CONFIG_SMARTSCORE_DOUBAO_VOICE_RECONNECT_MS
#define CONFIG_SMARTSCORE_DOUBAO_VOICE_RECONNECT_MS 3000
#endif
#ifndef DOUBAO_VOICE_API_KEY
#define DOUBAO_VOICE_API_KEY ""
#endif

#define INPUT_SAMPLE_RATE_HZ 16000U
#define OUTPUT_SAMPLE_RATE_HZ 24000U
#define INPUT_CHUNK_SAMPLES 800U
#define INPUT_CHUNK_BYTES (INPUT_CHUNK_SAMPLES * sizeof(int16_t))
#define INPUT_FAST_FLUSH_CHUNK_BYTES (16U * 1024U)
#define WORKER_TASK_STACK_BYTES 12288U
#define WORKER_TASK_PRIORITY 6U
#define WORKER_TICK_MS 20U
#define WS_SEND_TIMEOUT_MS 1000U
#define WS_BUFFER_BYTES 4096U
#define WS_TASK_STACK_BYTES 6144U
#define WS_TASK_PRIORITY 7U
#define MAX_RX_MESSAGE_BYTES (512U * 1024U)
#define RX_MESSAGE_INITIAL_BYTES (8U * 1024U)
#define MAX_CONTEXT_BYTES 512U
#define MAX_INSTRUCTIONS_BYTES 1200U
#define MAX_STOP_REASON_BYTES 64U
#define INPUT_CHUNK_DURATION_US 50000ULL
#define MAX_INPUT_FLUSH_CHUNKS_PER_TICK 8U
#define RESPONSE_WATCHDOG_DIAGNOSTIC_MS 15000ULL
#define AUDIO_DELTA_GAP_WARN_US 500000ULL
#define AUDIO_RATE_EVALUATION_MIN_US 1000000ULL
#define WS_AUDIO_CALLBACK_WARN_US 20000ULL
#define CLOUD_END_SMOOTH_WINDOW_MS 500U
#define TTS_PENDING_BUFFER_BYTES (128U * 1024U)
#define TTS_BACKPRESSURE_LOG_INTERVAL 50U
#define MAX_TTS_TRANSFERS_PER_TICK 8U
#define CONTROL_QUEUE_LENGTH 24U
#define PIPELINE_LOG_INTERVAL_US 1000000ULL
#define INPUT_BASE64_BUFFER_BYTES \
    ((((INPUT_FAST_FLUSH_CHUNK_BYTES) + 2U) / 3U) * 4U + 1U)
#define INPUT_JSON_BUFFER_BYTES (INPUT_BASE64_BUFFER_BYTES + 64U)
#define TLS_DMA_WARN_FREE_BYTES (48U * 1024U)
#define TLS_DMA_WARN_LARGEST_BLOCK_BYTES (12U * 1024U)
#define TLS_DMA_CRITICAL_FREE_BYTES (8U * 1024U)
#define TLS_DMA_CRITICAL_LARGEST_BLOCK_BYTES (2U * 1024U)
#define MAX_CONNECT_FAILURES 3U

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t read_offset;
    size_t write_offset;
    size_t used;
    SemaphoreHandle_t lock;
} pcm_ring_t;

typedef enum {
    VOICE_SESSION_IDLE = 0,
    VOICE_SESSION_CANDIDATE,
    VOICE_SESSION_LISTENING,
    VOICE_SESSION_WAIT_RESPONSE,
    VOICE_SESSION_SPEAKING,
    VOICE_SESSION_DRAINING,
} voice_session_state_t;

typedef struct {
    uint64_t total_input_bytes;
    uint64_t uploaded_input_bytes;
    size_t queued_before_bytes;
    size_t kept_tail_bytes;
    size_t discarded_tail_bytes;
} input_stop_stats_t;

typedef enum {
    VOICE_CONTROL_TRANSPORT_CONNECTED = 0,
    VOICE_CONTROL_TRANSPORT_DISCONNECTED,
    VOICE_CONTROL_TRANSPORT_ERROR,
    VOICE_CONTROL_TRANSPORT_PEER_CLOSED,
    VOICE_CONTROL_SESSION_CREATED,
    VOICE_CONTROL_INPUT_COMMITTED,
    VOICE_CONTROL_ASR_STARTED,
    VOICE_CONTROL_ASR_COMPLETED,
    VOICE_CONTROL_ASR_FAILED,
    VOICE_CONTROL_OUTPUT_STARTED,
    VOICE_CONTROL_OUTPUT_DONE,
    VOICE_CONTROL_RESPONSE_DONE,
    VOICE_CONTROL_SESSION_CLOSED,
    VOICE_CONTROL_AUDIO_FLOW,
} voice_control_type_t;

typedef struct {
    voice_control_type_t type;
    bool fatal;
    char reason[MAX_STOP_REASON_BYTES];
    uint32_t audio_delta_index;
    uint32_t decoded_pcm_bytes;
    uint32_t audio_delta_gap_ms;
    uint32_t callback_elapsed_ms;
    uint64_t audio_pcm_total_bytes;
    uint64_t average_pcm_bytes_per_second;
    uint64_t expected_pcm_bytes_per_second;
    bool upstream_audio_gap;
} voice_control_event_t;

typedef struct {
    uint32_t index;
    uint32_t decoded_pcm_bytes;
    uint32_t gap_ms;
    uint64_t total_pcm_bytes;
    uint64_t average_bytes_per_second;
    uint64_t expected_bytes_per_second;
    bool upstream_gap;
} audio_flow_stats_t;

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    size_t frame_expected;
    size_t frame_received;
    uint8_t opcode;
    bool active;
} ws_rx_accumulator_t;

typedef struct {
    uint64_t wake_us;
    uint64_t ai_begin_us;
    uint64_t session_created_us;
    uint64_t first_asr_us;
    uint64_t input_commit_us;
    uint64_t response_audio_started_us;
    uint64_t first_audio_delta_us;
    uint64_t first_speaker_playback_us;
    uint64_t output_audio_done_us;
    bool summary_logged;
} voice_latency_t;

static const char *TAG = "VOICE_ASSISTANT";
static pcm_ring_t s_input;
static QueueHandle_t s_control_queue;
static SemaphoreHandle_t s_tts_pending_lock;
static uint8_t *s_tts_pending_data;
static size_t s_tts_pending_offset;
static size_t s_tts_pending_bytes;
static uint8_t *s_input_send_pcm;
static char *s_input_send_base64;
static char *s_input_send_json;
static TaskHandle_t s_worker_task;
static StaticTask_t s_worker_task_control;
static StackType_t *s_worker_stack;
static esp_websocket_client_handle_t s_client;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static voice_session_state_t s_session_state = VOICE_SESSION_IDLE;
static bool s_network_ready;
static bool s_armed;
static bool s_session_requested;
static bool s_accept_input;
static bool s_committed;
static bool s_utterance_closed;
static bool s_connected;
static bool s_session_ready;
static bool s_session_create_sent;
static bool s_input_commit_sent;
static bool s_input_commit_acknowledged;
static bool s_cancel_response_requested;
static bool s_stop_requested;
static bool s_stop_is_error;
static bool s_report_connected;
static bool s_report_session_started;
static bool s_report_input_completed;
static bool s_response_audio_done;
static bool s_playback_seen_active;
static bool s_input_capacity_exhausted;
static uint64_t s_next_audio_send_us;
static bool s_output_start_pending;
static bool s_output_start_command_sent;
static bool s_output_stream_started;
static bool s_output_done_pending;
static bool s_response_done_received;
static bool s_response_watchdog_reported;
static bool s_session_close_pending;
static uint64_t s_session_close_deadline_us;
static uint64_t s_next_transport_attempt_us;
static bool s_api_key_missing_logged;
static uint32_t s_event_id;
static uint32_t s_audio_sent_chunks;
static uint32_t s_input_overruns;
static uint64_t s_input_total_pcm_bytes;
static uint64_t s_input_uploaded_pcm_bytes;
static uint64_t s_tts_received_pcm_bytes;
static uint64_t s_tts_pumped_pcm_bytes;
static size_t s_tts_max_buffered_bytes;
static uint32_t s_tts_backpressure_events;
static uint32_t s_tts_overflow_events;
static uint32_t s_connect_failures;
static uint64_t s_next_pipeline_log_us;
static uint64_t s_last_any_business_rx_ms;
static uint64_t s_last_audio_delta_ms;
static uint64_t s_audio_flow_last_delta_us;
static uint64_t s_audio_flow_start_us;
static uint64_t s_audio_flow_total_pcm_bytes;
static uint32_t s_audio_delta_index;
static uint32_t s_output_sample_rate_hz = OUTPUT_SAMPLE_RATE_HZ;
static uint8_t s_output_bits = 16U;
static uint8_t s_output_channels = 1U;
static char s_output_codec[24] = "pcm_s16le";
static char s_stop_reason[MAX_STOP_REASON_BYTES];
static char s_last_client_event[MAX_STOP_REASON_BYTES];
static char s_last_server_event[MAX_STOP_REASON_BYTES];
static voice_latency_t s_latency;
static char s_ws_headers[384];
static ws_rx_accumulator_t s_ws_rx;
static voice_assistant_event_handler_t s_event_handler;
static void *s_event_context;
static voice_assistant_context_provider_t s_context_provider;
static void *s_context_provider_context;

static void notify_worker(void)
{
    if (s_worker_task != NULL) xTaskNotifyGive(s_worker_task);
}

static void queue_control_event(voice_control_type_t type,
                                bool fatal,
                                const char *reason)
{
    if (s_control_queue == NULL) return;
    voice_control_event_t event = {
        .type = type,
        .fatal = fatal,
    };
    strlcpy(event.reason, reason != NULL ? reason : "",
            sizeof(event.reason));
    if (xQueueSend(s_control_queue, &event, 0) != pdTRUE) {
        ESP_LOGE(TAG, "voice control queue full; event=%d", (int)type);
        return;
    }
    notify_worker();
}

static void queue_audio_flow_event(const audio_flow_stats_t *flow,
                                   uint32_t callback_elapsed_ms)
{
    if (s_control_queue == NULL || flow == NULL) return;
    voice_control_event_t event = {
        .type = VOICE_CONTROL_AUDIO_FLOW,
        .audio_delta_index = flow->index,
        .decoded_pcm_bytes = flow->decoded_pcm_bytes,
        .audio_delta_gap_ms = flow->gap_ms,
        .callback_elapsed_ms = callback_elapsed_ms,
        .audio_pcm_total_bytes = flow->total_pcm_bytes,
        .average_pcm_bytes_per_second = flow->average_bytes_per_second,
        .expected_pcm_bytes_per_second = flow->expected_bytes_per_second,
        .upstream_audio_gap = flow->upstream_gap,
    };
    if (xQueueSend(s_control_queue, &event, 0) != pdTRUE) {
        ESP_LOGE(TAG, "voice control queue full; audio flow idx=%lu lost",
                 (unsigned long)flow->index);
        return;
    }
    notify_worker();
}

static void tts_pending_reset(void)
{
    if (s_tts_pending_lock == NULL) return;
    if (xSemaphoreTake(s_tts_pending_lock,
                       pdMS_TO_TICKS(20)) == pdTRUE) {
        s_tts_pending_offset = 0U;
        s_tts_pending_bytes = 0U;
        s_tts_max_buffered_bytes = 0U;
        xSemaphoreGive(s_tts_pending_lock);
    }
}

static void ws_rx_reset(bool release_buffer)
{
    s_ws_rx.length = 0U;
    s_ws_rx.frame_expected = 0U;
    s_ws_rx.frame_received = 0U;
    s_ws_rx.opcode = 0U;
    s_ws_rx.active = false;
    if (release_buffer) {
        free(s_ws_rx.data);
        s_ws_rx.data = NULL;
        s_ws_rx.capacity = 0U;
    }
}

static bool ws_rx_reserve(size_t required)
{
    if (required > MAX_RX_MESSAGE_BYTES + 1U) return false;
    if (required <= s_ws_rx.capacity) return true;
    size_t capacity = s_ws_rx.capacity > 0U
                          ? s_ws_rx.capacity
                          : RX_MESSAGE_INITIAL_BYTES;
    while (capacity < required) {
        const size_t next = capacity <= MAX_RX_MESSAGE_BYTES / 2U
                                ? capacity * 2U
                                : MAX_RX_MESSAGE_BYTES + 1U;
        if (next <= capacity) return false;
        capacity = next;
    }
    if (capacity > MAX_RX_MESSAGE_BYTES + 1U) {
        capacity = MAX_RX_MESSAGE_BYTES + 1U;
    }
    char *grown = heap_caps_realloc(
        s_ws_rx.data, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (grown == NULL) return false;
    s_ws_rx.data = grown;
    s_ws_rx.capacity = capacity;
    return true;
}

static size_t tts_pending_used(void)
{
    if (s_tts_pending_lock == NULL) return 0U;
    if (xSemaphoreTake(s_tts_pending_lock,
                       pdMS_TO_TICKS(2)) != pdTRUE) {
        return s_tts_pending_bytes;
    }
    const size_t used = s_tts_pending_bytes;
    xSemaphoreGive(s_tts_pending_lock);
    return used;
}

static bool tts_pending_append_locked(const uint8_t *data, size_t bytes)
{
    if (bytes == 0U) return true;
    if (data == NULL || s_tts_pending_data == NULL) return false;
    if (s_tts_pending_offset > 0U &&
        s_tts_pending_offset + s_tts_pending_bytes + bytes >
            TTS_PENDING_BUFFER_BYTES) {
        memmove(s_tts_pending_data,
                s_tts_pending_data + s_tts_pending_offset,
                s_tts_pending_bytes);
        s_tts_pending_offset = 0U;
    }
    if (bytes > TTS_PENDING_BUFFER_BYTES - s_tts_pending_offset -
                    s_tts_pending_bytes) {
        return false;
    }
    memcpy(s_tts_pending_data + s_tts_pending_offset +
               s_tts_pending_bytes,
           data, bytes);
    s_tts_pending_bytes += bytes;
    if (s_tts_pending_bytes > s_tts_max_buffered_bytes) {
        s_tts_max_buffered_bytes = s_tts_pending_bytes;
    }
    return true;
}

static const char *session_state_name(voice_session_state_t state)
{
    switch (state) {
    case VOICE_SESSION_IDLE:
        return "IDLE";
    case VOICE_SESSION_CANDIDATE:
        return "CANDIDATE";
    case VOICE_SESSION_LISTENING:
        return "LISTENING";
    case VOICE_SESSION_WAIT_RESPONSE:
        return "WAIT_RESPONSE";
    case VOICE_SESSION_SPEAKING:
        return "SPEAKING";
    case VOICE_SESSION_DRAINING:
        return "DRAINING";
    default:
        return "UNKNOWN";
    }
}

static void set_session_state(voice_session_state_t next,
                              const char *reason)
{
    voice_session_state_t previous;
    portENTER_CRITICAL(&s_state_lock);
    previous = s_session_state;
    s_session_state = next;
    portEXIT_CRITICAL(&s_state_lock);
    if (previous != next) {
        ESP_LOGI(TAG, "session state: %s -> %s reason=%s",
                 session_state_name(previous), session_state_name(next),
                 reason != NULL ? reason : "unspecified");
    }
}

static void mark_latency_once(uint64_t *field)
{
    if (field == NULL) return;
    const uint64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    if (*field == 0U) *field = now_us;
    portEXIT_CRITICAL(&s_state_lock);
}

static long long latency_from_wake_ms(uint64_t wake_us,
                                      uint64_t event_us)
{
    if (wake_us == 0U || event_us < wake_us) return -1LL;
    return (long long)((event_us - wake_us) / 1000ULL);
}

static void log_latency_summary(void)
{
    voice_latency_t latency;
    portENTER_CRITICAL(&s_state_lock);
    if (s_latency.summary_logged) {
        portEXIT_CRITICAL(&s_state_lock);
        return;
    }
    s_latency.summary_logged = true;
    latency = s_latency;
    portEXIT_CRITICAL(&s_state_lock);
    ESP_LOGI(TAG,
             "LATENCY: WAKE=0ms AI_BEGIN=%lldms session.created=%lldms first_ASR=%lldms input.commit=%lldms response.output_audio.started=%lldms first_audio.delta=%lldms first_speaker_playback=%lldms output_audio.done=%lldms",
             latency_from_wake_ms(latency.wake_us,
                                  latency.ai_begin_us),
             latency_from_wake_ms(latency.wake_us,
                                  latency.session_created_us),
             latency_from_wake_ms(latency.wake_us,
                                  latency.first_asr_us),
             latency_from_wake_ms(latency.wake_us,
                                  latency.input_commit_us),
             latency_from_wake_ms(latency.wake_us,
                                  latency.response_audio_started_us),
             latency_from_wake_ms(latency.wake_us,
                                  latency.first_audio_delta_us),
             latency_from_wake_ms(latency.wake_us,
                                  latency.first_speaker_playback_us),
             latency_from_wake_ms(latency.wake_us,
                                  latency.output_audio_done_us));
}

static bool log_memory_status(const char *stage, bool protect_critical_memory)
{
    const uint32_t dma_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA |
                              MALLOC_CAP_8BIT;
    const size_t internal_free = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t dma_free = heap_caps_get_free_size(dma_caps);
    const size_t dma_largest = heap_caps_get_largest_free_block(dma_caps);
    const size_t psram_free = heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG,
             "memory %s: internal=%u DMA=%u largest-DMA=%u PSRAM=%u bytes",
             stage != NULL ? stage : "status",
             (unsigned)internal_free, (unsigned)dma_free,
             (unsigned)dma_largest, (unsigned)psram_free);
    if (protect_critical_memory &&
        (dma_free < TLS_DMA_WARN_FREE_BYTES ||
         dma_largest < TLS_DMA_WARN_LARGEST_BLOCK_BYTES)) {
        ESP_LOGW(TAG,
                 "TLS memory below preferred reserve; attempting PSRAM-backed handshake: free=%u largest=%u",
                 (unsigned)dma_free, (unsigned)dma_largest);
    }
    if (protect_critical_memory &&
        (dma_free < TLS_DMA_CRITICAL_FREE_BYTES ||
         dma_largest < TLS_DMA_CRITICAL_LARGEST_BLOCK_BYTES)) {
        ESP_LOGE(TAG,
                 "TLS retry deferred at critical DMA level: need free>=%u largest>=%u bytes",
                 (unsigned)TLS_DMA_CRITICAL_FREE_BYTES,
                 (unsigned)TLS_DMA_CRITICAL_LARGEST_BLOCK_BYTES);
        return false;
    }
    return true;
}

static uint32_t record_connection_failure(void)
{
    uint32_t failures;
    portENTER_CRITICAL(&s_state_lock);
    failures = ++s_connect_failures;
    portEXIT_CRITICAL(&s_state_lock);
    return failures;
}

static void report_event(voice_assistant_event_type_t type,
                         esp_err_t error,
                         const char *reason)
{
    if (s_event_handler == NULL) return;
    const voice_assistant_event_t event = {
        .type = type,
        .error = error,
        .reason = reason,
    };
    s_event_handler(&event, s_event_context);
}

static void ring_reset(void)
{
    if (s_input.lock == NULL) return;
    if (xSemaphoreTake(s_input.lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_input.read_offset = 0;
        s_input.write_offset = 0;
        s_input.used = 0;
        xSemaphoreGive(s_input.lock);
    }
}

static bool ring_write_locked(const uint8_t *data, size_t length)
{
    if (length > s_input.capacity - s_input.write_offset) {
        ++s_input_overruns;
        if (!s_input_capacity_exhausted) {
            s_input_capacity_exhausted = true;
            ESP_LOGE(TAG,
                     "single-turn PCM capacity exhausted at %u bytes; tail audio discarded",
                     (unsigned)s_input.write_offset);
        }
        return false;
    }
    memcpy(s_input.data + s_input.write_offset, data, length);
    s_input.write_offset += length;
    s_input.used = s_input.write_offset - s_input.read_offset;
    return true;
}

static size_t ring_read(uint8_t *output, size_t capacity)
{
    if (xSemaphoreTake(s_input.lock, pdMS_TO_TICKS(5)) != pdTRUE) return 0;
    size_t pending = s_input.write_offset - s_input.read_offset;
    size_t length = pending < capacity ? pending : capacity;
    memcpy(output, s_input.data + s_input.read_offset, length);
    s_input.read_offset += length;
    s_input.used = s_input.write_offset - s_input.read_offset;
    xSemaphoreGive(s_input.lock);
    return length;
}

static size_t ring_used(void)
{
    if (xSemaphoreTake(s_input.lock, pdMS_TO_TICKS(5)) != pdTRUE) return 0;
    size_t used = s_input.used;
    xSemaphoreGive(s_input.lock);
    return used;
}

static size_t ring_captured(void)
{
    if (xSemaphoreTake(s_input.lock, pdMS_TO_TICKS(5)) != pdTRUE) return 0;
    size_t captured = s_input.write_offset;
    xSemaphoreGive(s_input.lock);
    return captured;
}

static void ring_unread(size_t bytes)
{
    if (xSemaphoreTake(s_input.lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (bytes > s_input.read_offset) bytes = s_input.read_offset;
        s_input.read_offset -= bytes;
        s_input.used = s_input.write_offset - s_input.read_offset;
        xSemaphoreGive(s_input.lock);
    }
}

static input_stop_stats_t stop_input_upload(const char *reason,
                                            size_t keep_tail_bytes)
{
    input_stop_stats_t stats = {0};
    portENTER_CRITICAL(&s_state_lock);
    s_accept_input = false;
    s_utterance_closed = true;
    stats.total_input_bytes = s_input_total_pcm_bytes;
    stats.uploaded_input_bytes = s_input_uploaded_pcm_bytes;
    portEXIT_CRITICAL(&s_state_lock);

    if (s_input.lock != NULL &&
        xSemaphoreTake(s_input.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        stats.queued_before_bytes =
            s_input.write_offset - s_input.read_offset;
        stats.kept_tail_bytes = stats.queued_before_bytes < keep_tail_bytes
                                    ? stats.queued_before_bytes
                                    : keep_tail_bytes;
        stats.discarded_tail_bytes =
            stats.queued_before_bytes - stats.kept_tail_bytes;
        s_input.write_offset = s_input.read_offset + stats.kept_tail_bytes;
        s_input.used = stats.kept_tail_bytes;
        xSemaphoreGive(s_input.lock);
    } else {
        ESP_LOGW(TAG,
                 "input upload stopped but pending PCM trim lock timed out");
    }

    ESP_LOGI(TAG,
             "input upload stopped: reason=%s total=%llu uploaded=%llu queued=%u kept-tail=%u discarded-tail=%u bytes",
             reason != NULL ? reason : "unspecified",
             (unsigned long long)stats.total_input_bytes,
             (unsigned long long)stats.uploaded_input_bytes,
             (unsigned)stats.queued_before_bytes,
             (unsigned)stats.kept_tail_bytes,
             (unsigned)stats.discarded_tail_bytes);
    notify_worker();
    return stats;
}

static void log_tts_ring_metrics(const char *stage)
{
    uint64_t received;
    uint64_t pumped;
    uint32_t backpressure;
    uint32_t overflow;
    portENTER_CRITICAL(&s_state_lock);
    received = s_tts_received_pcm_bytes;
    pumped = s_tts_pumped_pcm_bytes;
    backpressure = s_tts_backpressure_events;
    overflow = s_tts_overflow_events;
    portEXIT_CRITICAL(&s_state_lock);
    const size_t buffered = tts_pending_used();
    size_t maximum = 0U;
    if (s_tts_pending_lock != NULL &&
        xSemaphoreTake(s_tts_pending_lock,
                       pdMS_TO_TICKS(2)) == pdTRUE) {
        maximum = s_tts_max_buffered_bytes;
        xSemaphoreGive(s_tts_pending_lock);
    }
    speaker_stream_metrics_t speaker;
    speaker_service_stream_get_metrics(&speaker);
    ESP_LOGI(TAG,
             "PCM pipeline %s: tts_ring=direct pending=%u/%u max-pending=%u speaker_ring=%u/%u received=%llu pumped=%llu played=%llu underrun=%lu backpressure=%lu overflow=%lu",
             stage != NULL ? stage : "status",
             (unsigned)buffered, (unsigned)TTS_PENDING_BUFFER_BYTES,
             (unsigned)maximum, (unsigned)speaker.buffered_bytes,
             (unsigned)speaker.capacity_bytes,
             (unsigned long long)received,
             (unsigned long long)pumped,
             (unsigned long long)speaker.played_bytes,
             (unsigned long)speaker.underruns,
             (unsigned long)backpressure, (unsigned long)overflow);
}

static void request_stop(const char *reason, bool error)
{
    portENTER_CRITICAL(&s_state_lock);
    s_accept_input = false;
    s_stop_requested = true;
    s_stop_is_error = error;
    strlcpy(s_stop_reason, reason != NULL ? reason : "session ended",
            sizeof(s_stop_reason));
    portEXIT_CRITICAL(&s_state_lock);
    notify_worker();
}

static void record_client_event(const char *type, size_t pcm_bytes)
{
    const uint64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    strlcpy(s_last_client_event, type != NULL ? type : "unknown",
            sizeof(s_last_client_event));
    portEXIT_CRITICAL(&s_state_lock);
    ESP_LOGI(TAG,
             "CLIENT_EVENT type=%s time=%llu ms pcm=%u bytes",
             type != NULL ? type : "unknown",
             (unsigned long long)(now_us / 1000ULL),
             (unsigned)pcm_bytes);
}

static void reset_cloud_session_metrics(void)
{
    tts_pending_reset();
    const uint64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    s_audio_sent_chunks = 0U;
    s_input_uploaded_pcm_bytes = 0U;
    s_tts_received_pcm_bytes = 0U;
    s_tts_pumped_pcm_bytes = 0U;
    s_tts_backpressure_events = 0U;
    s_tts_overflow_events = 0U;
    s_audio_delta_index = 0U;
    s_audio_flow_start_us = 0U;
    s_audio_flow_total_pcm_bytes = 0U;
    s_last_audio_delta_ms = 0U;
    s_audio_flow_last_delta_us = 0U;
    s_last_any_business_rx_ms = now_us / 1000ULL;
    s_response_watchdog_reported = false;
    s_output_sample_rate_hz = OUTPUT_SAMPLE_RATE_HZ;
    s_output_bits = 16U;
    s_output_channels = 1U;
    strlcpy(s_output_codec, "pcm_s16le", sizeof(s_output_codec));
    portEXIT_CRITICAL(&s_state_lock);
}

static int send_text(const char *text)
{
    if (s_client == NULL || text == NULL ||
        !esp_websocket_client_is_connected(s_client)) {
        return -1;
    }
    return esp_websocket_client_send_text(
        s_client, text, (int)strlen(text),
        pdMS_TO_TICKS(WS_SEND_TIMEOUT_MS));
}

static esp_err_t send_simple_event(const char *type)
{
    char json[128];
    uint32_t event_id;
    portENTER_CRITICAL(&s_state_lock);
    event_id = ++s_event_id;
    portEXIT_CRITICAL(&s_state_lock);
    int length = snprintf(json, sizeof(json),
                          "{\"type\":\"%s\",\"event_id\":\"p4-%lu\"}",
                          type, (unsigned long)event_id);
    if (length <= 0 || length >= (int)sizeof(json)) return ESP_FAIL;
    if (send_text(json) != length) return ESP_FAIL;
    record_client_event(type, 0U);
    return ESP_OK;
}

static void format_uuid(char output[37])
{
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = (uint8_t)((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = (uint8_t)((bytes[8] & 0x3FU) | 0x80U);
    snprintf(output, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             (unsigned)bytes[0], (unsigned)bytes[1],
             (unsigned)bytes[2], (unsigned)bytes[3],
             (unsigned)bytes[4], (unsigned)bytes[5],
             (unsigned)bytes[6], (unsigned)bytes[7],
             (unsigned)bytes[8], (unsigned)bytes[9],
             (unsigned)bytes[10], (unsigned)bytes[11],
             (unsigned)bytes[12], (unsigned)bytes[13],
             (unsigned)bytes[14], (unsigned)bytes[15]);
}

static esp_err_t send_session_create(void)
{
    char context[MAX_CONTEXT_BYTES] = {0};
    if (s_context_provider != NULL) {
        esp_err_t context_error = s_context_provider(
            context, sizeof(context), s_context_provider_context);
        if (context_error != ESP_OK) {
            ESP_LOGW(TAG, "context snapshot unavailable: %s",
                     esp_err_to_name(context_error));
            context[0] = '\0';
        }
    }
    char instructions[MAX_INSTRUCTIONS_BYTES];
    if (context[0] != '\0') {
        snprintf(instructions, sizeof(instructions), "%s\nDevice context: %s",
                 CONFIG_SMARTSCORE_DOUBAO_VOICE_INSTRUCTIONS, context);
    } else {
        strlcpy(instructions,
                CONFIG_SMARTSCORE_DOUBAO_VOICE_INSTRUCTIONS,
                sizeof(instructions));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *session = cJSON_CreateObject();
    cJSON *audio = cJSON_CreateObject();
    cJSON *input = cJSON_CreateObject();
    cJSON *input_format = cJSON_CreateObject();
    cJSON *output = cJSON_CreateObject();
    cJSON *output_format = cJSON_CreateObject();
    cJSON *asr = cJSON_CreateObject();
    cJSON *asr_extra = cJSON_CreateObject();
    if (root == NULL || session == NULL || audio == NULL || input == NULL ||
        input_format == NULL || output == NULL || output_format == NULL ||
        asr == NULL || asr_extra == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(session);
        cJSON_Delete(audio);
        cJSON_Delete(input);
        cJSON_Delete(input_format);
        cJSON_Delete(output);
        cJSON_Delete(output_format);
        cJSON_Delete(asr);
        cJSON_Delete(asr_extra);
        return ESP_ERR_NO_MEM;
    }

    char session_id[37];
    char event_id[32];
    format_uuid(session_id);
    portENTER_CRITICAL(&s_state_lock);
    snprintf(event_id, sizeof(event_id), "p4-%lu",
             (unsigned long)++s_event_id);
    portEXIT_CRITICAL(&s_state_lock);

    cJSON_AddStringToObject(root, "type", "session.create");
    cJSON_AddStringToObject(root, "event_id", event_id);
    cJSON_AddStringToObject(session, "id", session_id);
    cJSON_AddStringToObject(session, "model",
                            CONFIG_SMARTSCORE_DOUBAO_VOICE_MODEL);
    cJSON_AddStringToObject(session, "instructions", instructions);
    cJSON_AddStringToObject(input_format, "type", "pcm");
    cJSON_AddNumberToObject(input_format, "rate", INPUT_SAMPLE_RATE_HZ);
    cJSON_AddItemToObject(input, "format", input_format);
    cJSON_AddStringToObject(output_format, "type", "pcm_s16le");
    cJSON_AddNumberToObject(output_format, "rate", OUTPUT_SAMPLE_RATE_HZ);
    cJSON_AddItemToObject(output, "format", output_format);
    cJSON_AddStringToObject(output, "voice",
                            CONFIG_SMARTSCORE_DOUBAO_VOICE_NAME);
    cJSON_AddNumberToObject(asr_extra, "end_smooth_window_ms",
                            CLOUD_END_SMOOTH_WINDOW_MS);
    cJSON_AddItemToObject(asr, "extra", asr_extra);
    cJSON_AddItemToObject(audio, "input", input);
    cJSON_AddItemToObject(audio, "output", output);
    cJSON_AddItemToObject(session, "audio", audio);
    cJSON_AddItemToObject(session, "asr", asr);
    cJSON_AddItemToObject(root, "session", session);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) return ESP_ERR_NO_MEM;
    const int length = (int)strlen(json);
    int sent = send_text(json);
    cJSON_free(json);
    if (sent != length) return ESP_FAIL;
    record_client_event("session.create", 0U);
    ESP_LOGI(TAG,
             "session.create sent: model=%s input=pcm/16k output=pcm_s16le/24k end-smooth=%u ms",
             CONFIG_SMARTSCORE_DOUBAO_VOICE_MODEL,
             (unsigned)CLOUD_END_SMOOTH_WINDOW_MS);
    return ESP_OK;
}

static esp_err_t send_audio_chunk(const uint8_t *pcm, size_t pcm_length)
{
    const size_t base64_capacity = ((pcm_length + 2U) / 3U) * 4U + 1U;
    if (s_input_send_base64 == NULL || s_input_send_json == NULL ||
        base64_capacity > INPUT_BASE64_BUFFER_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t encoded_length = 0;
    int rc = mbedtls_base64_encode(
        (unsigned char *)s_input_send_base64,
        INPUT_BASE64_BUFFER_BYTES, &encoded_length,
        pcm, pcm_length);
    if (rc != 0) return ESP_FAIL;
    s_input_send_base64[encoded_length] = '\0';

    int length = snprintf(s_input_send_json, INPUT_JSON_BUFFER_BYTES,
                          "{\"type\":\"input_audio_buffer.append\","
                          "\"audio\":\"%s\"}", s_input_send_base64);
    if (length <= 0 || length >= (int)INPUT_JSON_BUFFER_BYTES) {
        return ESP_FAIL;
    }
    if (send_text(s_input_send_json) != length) return ESP_FAIL;
    return ESP_OK;
}

static const char *json_string(cJSON *object, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static uint32_t json_u32(cJSON *object, const char *name)
{
    if (!cJSON_IsObject(object) || name == NULL) return 0U;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || item->valuedouble <= 0.0 ||
        item->valuedouble > (double)UINT32_MAX) {
        return 0U;
    }
    return (uint32_t)item->valuedouble;
}

static cJSON *json_object(cJSON *parent, const char *name)
{
    if (!cJSON_IsObject(parent) || name == NULL) return NULL;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, name);
    return cJSON_IsObject(item) ? item : NULL;
}

static bool parse_session_created_output_format(cJSON *root)
{
    char codec[sizeof(s_output_codec)] = "pcm_s16le";
    uint32_t rate = OUTPUT_SAMPLE_RATE_HZ;
    uint32_t bits = 16U;
    uint32_t channels = 1U;
    bool echoed = false;

    cJSON *session = json_object(root, "session");
    cJSON *audio = json_object(session, "audio");
    cJSON *output = json_object(audio, "output");
    cJSON *format = json_object(output, "format");
    if (format != NULL) {
        const char *server_codec = json_string(format, "type");
        uint32_t server_rate = json_u32(format, "rate");
        if (server_rate == 0U) {
            server_rate = json_u32(format, "sample_rate");
        }
        if (server_rate == 0U) {
            server_rate = json_u32(format, "sample_rate_hz");
        }
        uint32_t server_bits = json_u32(format, "bits");
        if (server_bits == 0U) {
            server_bits = json_u32(format, "bits_per_sample");
        }
        uint32_t server_channels = json_u32(format, "channels");
        if (server_channels == 0U) {
            server_channels = json_u32(output, "channels");
        }
        if (server_codec != NULL && server_codec[0] != '\0') {
            strlcpy(codec, server_codec, sizeof(codec));
            echoed = true;
        }
        if (server_rate > 0U) {
            rate = server_rate;
            echoed = true;
        }
        if (server_bits > 0U) {
            bits = server_bits;
            echoed = true;
        }
        if (server_channels > 0U) {
            channels = server_channels;
            echoed = true;
        }
    }

    portENTER_CRITICAL(&s_state_lock);
    strlcpy(s_output_codec, codec, sizeof(s_output_codec));
    s_output_sample_rate_hz = rate;
    s_output_bits = (uint8_t)bits;
    s_output_channels = (uint8_t)channels;
    portEXIT_CRITICAL(&s_state_lock);

    ESP_LOGI(TAG,
             "DOUBAO_AUDIO_NEGOTIATED: codec=%s sample_rate=%lu bits=%lu channels=%lu source=%s",
             codec, (unsigned long)rate, (unsigned long)bits,
             (unsigned long)channels,
             echoed ? "session.created" :
                      "session.created-ack+official-v3-fixed-format");
    if (!echoed) {
        ESP_LOGI(TAG,
                 "session.created did not echo audio.format; official V3 fixes PCM output at 24kHz, using the acknowledged session.create format");
    }

    if (strcmp(codec, "pcm_s16le") != 0 || bits != 16U || channels != 1U ||
        rate < 8000U || rate > 48000U) {
        ESP_LOGE(TAG,
                 "unsupported negotiated output: codec=%s rate=%lu bits=%lu channels=%lu",
                 codec, (unsigned long)rate, (unsigned long)bits,
                 (unsigned long)channels);
        return false;
    }
    return true;
}

static void json_scalar_text(cJSON *item, char *output,
                             size_t output_capacity)
{
    if (output == NULL || output_capacity == 0U) return;
    output[0] = '\0';
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strlcpy(output, item->valuestring, output_capacity);
    } else if (cJSON_IsNumber(item)) {
        snprintf(output, output_capacity, "%g", item->valuedouble);
    } else if (cJSON_IsBool(item)) {
        strlcpy(output, cJSON_IsTrue(item) ? "true" : "false",
                output_capacity);
    }
}

static void record_server_event(cJSON *root, const char *type)
{
    const uint64_t now_us = esp_timer_get_time();
    const bool business_event = type != NULL &&
        strcmp(type, "ping") != 0 && strcmp(type, "pong") != 0;
    if (business_event) {
        portENTER_CRITICAL(&s_state_lock);
        s_last_any_business_rx_ms = now_us / 1000ULL;
        s_response_watchdog_reported = false;
        strlcpy(s_last_server_event, type, sizeof(s_last_server_event));
        portEXIT_CRITICAL(&s_state_lock);
    }
    if (type == NULL || strcmp(type, "error") != 0) return;

    char session_id[64] = {0};
    char status[64] = {0};
    char error_text[128] = {0};
    const char *event_id = json_string(root, "event_id");
    const char *direct_session_id = json_string(root, "session_id");
    cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session");
    const char *nested_session_id = cJSON_IsObject(session)
                                        ? json_string(session, "id")
                                        : NULL;
    strlcpy(session_id,
            direct_session_id != NULL ? direct_session_id
                                      : (nested_session_id != NULL
                                             ? nested_session_id : "-"),
            sizeof(session_id));

    cJSON *response = cJSON_GetObjectItemCaseSensitive(root, "response");
    cJSON *status_item = cJSON_GetObjectItemCaseSensitive(root,
                                                          "status_code");
    if (status_item == NULL) {
        status_item = cJSON_GetObjectItemCaseSensitive(root, "status");
    }
    if (status_item == NULL && cJSON_IsObject(response)) {
        status_item = cJSON_GetObjectItemCaseSensitive(response,
                                                        "status_code");
        if (status_item == NULL) {
            status_item = cJSON_GetObjectItemCaseSensitive(response,
                                                            "status");
        }
    }
    json_scalar_text(status_item, status, sizeof(status));
    if (status[0] == '\0') strlcpy(status, "-", sizeof(status));

    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsString(error)) {
        json_scalar_text(error, error_text, sizeof(error_text));
    } else if (cJSON_IsObject(error)) {
        const char *code = json_string(error, "code");
        const char *message = json_string(error, "message");
        snprintf(error_text, sizeof(error_text), "%s%s%s",
                 code != NULL ? code : "",
                 code != NULL && message != NULL ? ":" : "",
                 message != NULL ? message : "");
    }
    if (error_text[0] == '\0') {
        const char *message = json_string(root, "message");
        strlcpy(error_text, message != NULL ? message : "-",
                sizeof(error_text));
    }

    ESP_LOGE(TAG,
             "DOUBAO_EVENT: type=%s event_id=%s session_id=%s response/status=%s error=%s time=%llu ms",
             type, event_id != NULL ? event_id : "-", session_id,
             status, error_text,
             (unsigned long long)(now_us / 1000ULL));
}

static bool handle_audio_delta(cJSON *root, audio_flow_stats_t *flow)
{
    if (flow == NULL) return false;
    memset(flow, 0, sizeof(*flow));
    bool accept_output;
    portENTER_CRITICAL(&s_state_lock);
    accept_output = s_session_requested &&
                    !s_stop_requested;
    portEXIT_CRITICAL(&s_state_lock);
    if (!accept_output) return false;
    const char *delta = json_string(root, "delta");
    if (delta == NULL) return false;
    const uint64_t delta_rx_us = esp_timer_get_time();
    uint64_t previous_delta_us;
    uint64_t flow_start_us;
    uint64_t expected_bytes_per_second;
    portENTER_CRITICAL(&s_state_lock);
    previous_delta_us = s_audio_flow_last_delta_us;
    s_audio_flow_last_delta_us = delta_rx_us;
    s_last_audio_delta_ms = delta_rx_us / 1000ULL;
    if (s_audio_flow_start_us == 0U) {
        s_audio_flow_start_us = delta_rx_us;
    }
    flow_start_us = s_audio_flow_start_us;
    flow->index = ++s_audio_delta_index;
    expected_bytes_per_second =
        ((uint64_t)s_output_sample_rate_hz * s_output_bits *
         s_output_channels) / 8ULL;
    portEXIT_CRITICAL(&s_state_lock);

    const size_t encoded_length = strlen(delta);
    size_t capacity = (encoded_length * 3U) / 4U + 3U;
    uint8_t *pcm = heap_caps_malloc(
        capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm == NULL) {
        pcm = heap_caps_malloc(capacity, MALLOC_CAP_8BIT);
    }
    if (pcm == NULL) {
        ESP_LOGE(TAG, "TTS base64 buffer allocation failed: %u",
                 (unsigned)capacity);
        return false;
    }
    size_t pcm_length = 0;
    int rc = mbedtls_base64_decode(pcm, capacity, &pcm_length,
                                   (const unsigned char *)delta,
                                   encoded_length);
    if (rc != 0 || (pcm_length & 1U) != 0U) {
        ESP_LOGW(TAG, "invalid TTS base64/PCM block: rc=%d bytes=%u",
                 rc, (unsigned)pcm_length);
        free(pcm);
        return false;
    }
    bool queued = false;
    bool queue_busy = false;
    if (s_tts_pending_lock == NULL ||
        xSemaphoreTake(s_tts_pending_lock, 0) != pdTRUE) {
        queue_busy = true;
    } else {
        queued = tts_pending_append_locked(pcm, pcm_length);
        xSemaphoreGive(s_tts_pending_lock);
    }
    free(pcm);
    if (!queued) {
        portENTER_CRITICAL(&s_state_lock);
        ++s_tts_overflow_events;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGE(TAG,
                 "PCM queue %s: chunk=%u pending=%u/%u",
                 queue_busy ? "busy" : "overflow", (unsigned)pcm_length,
                 (unsigned)tts_pending_used(),
                 (unsigned)TTS_PENDING_BUFFER_BYTES);
        queue_control_event(VOICE_CONTROL_TRANSPORT_ERROR, true,
                            queue_busy ? "tts-queue-busy"
                                       : "tts-pending-overflow");
        return false;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_tts_received_pcm_bytes += pcm_length;
    s_audio_flow_total_pcm_bytes += pcm_length;
    const uint64_t total_pcm_bytes = s_audio_flow_total_pcm_bytes;
    portEXIT_CRITICAL(&s_state_lock);

    const uint64_t elapsed_us = delta_rx_us > flow_start_us
                                    ? delta_rx_us - flow_start_us : 0U;
    const uint64_t gap_us = previous_delta_us > 0U &&
                            delta_rx_us > previous_delta_us
                                ? delta_rx_us - previous_delta_us : 0U;
    const uint64_t average_bytes_per_second = elapsed_us > 0U
        ? total_pcm_bytes * 1000000ULL / elapsed_us : 0U;
    flow->decoded_pcm_bytes = (uint32_t)pcm_length;
    flow->gap_ms = (uint32_t)(gap_us / 1000ULL);
    flow->total_pcm_bytes = total_pcm_bytes;
    flow->average_bytes_per_second = average_bytes_per_second;
    flow->expected_bytes_per_second = expected_bytes_per_second;
    flow->upstream_gap =
        (previous_delta_us > 0U && gap_us > AUDIO_DELTA_GAP_WARN_US) ||
        (elapsed_us >= AUDIO_RATE_EVALUATION_MIN_US &&
         expected_bytes_per_second > 0U &&
         average_bytes_per_second < expected_bytes_per_second);
    notify_worker();
    return true;
}

static void handle_server_message(const char *message)
{
    const uint64_t callback_start_us = esp_timer_get_time();
    audio_flow_stats_t audio_flow = {0};
    bool audio_flow_ready = false;
    cJSON *root = cJSON_Parse(message);
    if (root == NULL) {
        ESP_LOGW(TAG, "invalid JSON from Doubao");
        return;
    }
    const char *type = json_string(root, "type");
    if (type == NULL) {
        ESP_LOGW(TAG, "Doubao JSON event missing type");
        cJSON_Delete(root);
        return;
    }
    record_server_event(root, type);

    if (strcmp(type, "session.created") == 0) {
        if (parse_session_created_output_format(root)) {
            ESP_LOGI(TAG, "Doubao session created");
            queue_control_event(VOICE_CONTROL_SESSION_CREATED, false, NULL);
        } else {
            queue_control_event(VOICE_CONTROL_TRANSPORT_ERROR, true,
                                "unsupported-output-format");
        }
    } else if (strcmp(type, "input_audio_buffer.committed") == 0) {
        queue_control_event(VOICE_CONTROL_INPUT_COMMITTED, false,
                            "input_audio_buffer.committed");
    } else if (strcmp(type, "conversation.item.input_audio_transcription.started") == 0) {
        queue_control_event(VOICE_CONTROL_ASR_STARTED, false, NULL);
    } else if (strcmp(type, "conversation.item.input_audio_transcription.delta") == 0) {
        mark_latency_once(&s_latency.first_asr_us);
    } else if (strcmp(type, "conversation.item.input_audio_transcription.completed") == 0) {
        const char *transcript = json_string(root, "transcript");
        ESP_LOGI(TAG, "ASR completed: %s",
                 transcript != NULL ? transcript : "");
        queue_control_event(VOICE_CONTROL_ASR_COMPLETED, false,
                            "asr-final");
    } else if (strcmp(type, "conversation.item.input_audio_transcription.failed") == 0) {
        queue_control_event(VOICE_CONTROL_ASR_FAILED, true, "asr-failed");
    } else if (strcmp(type, "response.output_text.delta") == 0) {
        /* Text deltas are intentionally silent on the real-time RX path. */
    } else if (strcmp(type, "response.output_text.done") == 0) {
        const char *text = json_string(root, "text");
        ESP_LOGI(TAG, "assistant: %s", text != NULL ? text : "");
    } else if (strcmp(type, "response.output_audio.started") == 0) {
        ESP_LOGI(TAG,
                 "Doubao audio response started; direct speaker ring requested");
        queue_control_event(VOICE_CONTROL_OUTPUT_STARTED, false, NULL);
    } else if (strcmp(type, "response.output_audio.delta") == 0) {
        mark_latency_once(&s_latency.first_audio_delta_us);
        audio_flow_ready = handle_audio_delta(root, &audio_flow);
    } else if (strcmp(type, "response.output_audio.done") == 0) {
        ESP_LOGI(TAG,
                 "Doubao audio response done; waiting for pending PCM, speaker ring and I2S to drain");
        queue_control_event(VOICE_CONTROL_OUTPUT_DONE, false, NULL);
    } else if (strcmp(type, "response.done") == 0) {
        queue_control_event(VOICE_CONTROL_RESPONSE_DONE, false,
                            "response.done");
    } else if (strcmp(type, "response.canceled") == 0) {
        ESP_LOGI(TAG, "Doubao response canceled");
    } else if (strcmp(type, "session.closed") == 0) {
        queue_control_event(VOICE_CONTROL_SESSION_CLOSED, false,
                            "session.closed");
    } else if (strcmp(type, "error") == 0) {
        cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        const char *code = error != NULL ? json_string(error, "code") : NULL;
        const char *reason = error != NULL ? json_string(error, "message") : NULL;
        ESP_LOGE(TAG, "Doubao protocol error: code=%s reason=%s",
                 code != NULL ? code : "unknown",
                 reason != NULL ? reason : "unknown");
        queue_control_event(VOICE_CONTROL_TRANSPORT_ERROR, true,
                            reason != NULL ? reason : "protocol-error");
    } else {
        ESP_LOGD(TAG, "unhandled Doubao event type=%s", type);
    }
    cJSON_Delete(root);
    if (audio_flow_ready) {
        const uint64_t callback_elapsed_us =
            esp_timer_get_time() - callback_start_us;
        queue_audio_flow_event(
            &audio_flow,
            (uint32_t)((callback_elapsed_us + 999ULL) / 1000ULL));
    }
}

static void handle_complete_ws_message(uint8_t opcode,
                                       char *message,
                                       size_t length)
{
    if (message == NULL || length == 0U) return;
    message[length] = '\0';
    if (opcode == 0x01U || message[0] == '{' || message[0] == '[') {
        handle_server_message(message);
        return;
    }
    ESP_LOGW(TAG,
             "complete binary WebSocket message is not JSON: bytes=%u opcode=0x%02x",
             (unsigned)length, (unsigned)opcode);
}

static void handle_ws_data_event(const esp_websocket_event_data_t *data)
{
    if (data == NULL) return;
    if (data->op_code >= 0x08U) return;
    if (data->data_len < 0 || data->payload_len < 0 ||
        data->payload_offset < 0 ||
        (data->data_len > 0 && data->data_ptr == NULL)) {
        ESP_LOGE(TAG, "invalid WebSocket data metadata; assembler reset");
        ws_rx_reset(false);
        return;
    }

    const size_t chunk_bytes = (size_t)data->data_len;
    const size_t frame_offset = (size_t)data->payload_offset;
    const size_t frame_bytes = data->payload_len > 0
                                   ? (size_t)data->payload_len
                                   : chunk_bytes;
    if (frame_offset == 0U) {
        if (data->op_code == 0x01U || data->op_code == 0x02U) {
            if (s_ws_rx.active && s_ws_rx.length > 0U) {
                ESP_LOGE(TAG,
                         "new WebSocket message before prior FIN; old=%u bytes reset",
                         (unsigned)s_ws_rx.length);
                ws_rx_reset(false);
            }
            s_ws_rx.active = true;
            s_ws_rx.opcode = data->op_code;
            s_ws_rx.length = 0U;
        } else if (data->op_code == 0x00U) {
            if (!s_ws_rx.active) {
                ESP_LOGE(TAG,
                         "continuation WebSocket frame without active message");
                ws_rx_reset(false);
                return;
            }
        } else {
            ESP_LOGW(TAG, "unsupported WebSocket opcode=0x%02x",
                     (unsigned)data->op_code);
            ws_rx_reset(false);
            return;
        }
        s_ws_rx.frame_expected = frame_bytes;
        s_ws_rx.frame_received = 0U;
    } else if (!s_ws_rx.active) {
        ESP_LOGE(TAG, "WebSocket frame tail without active message");
        ws_rx_reset(false);
        return;
    }

    if (frame_offset != s_ws_rx.frame_received ||
        frame_offset > frame_bytes || chunk_bytes > frame_bytes - frame_offset) {
        ESP_LOGE(TAG,
                 "WebSocket frame order error: offset=%u expected=%u chunk=%u frame=%u",
                 (unsigned)frame_offset,
                 (unsigned)s_ws_rx.frame_received,
                 (unsigned)chunk_bytes, (unsigned)frame_bytes);
        ws_rx_reset(false);
        return;
    }
    if (chunk_bytes > MAX_RX_MESSAGE_BYTES ||
        s_ws_rx.length > MAX_RX_MESSAGE_BYTES - chunk_bytes ||
        !ws_rx_reserve(s_ws_rx.length + chunk_bytes + 1U)) {
        ESP_LOGE(TAG,
                 "WebSocket message exceeds PSRAM assembler limit: current=%u chunk=%u max=%u",
                 (unsigned)s_ws_rx.length, (unsigned)chunk_bytes,
                 (unsigned)MAX_RX_MESSAGE_BYTES);
        ws_rx_reset(false);
        return;
    }
    if (chunk_bytes > 0U) {
        memcpy(s_ws_rx.data + s_ws_rx.length, data->data_ptr, chunk_bytes);
        s_ws_rx.length += chunk_bytes;
        s_ws_rx.frame_received += chunk_bytes;
    }

    const bool frame_complete =
        s_ws_rx.frame_received == s_ws_rx.frame_expected;
    if (!frame_complete) return;
    s_ws_rx.frame_expected = 0U;
    s_ws_rx.frame_received = 0U;
    if (!data->fin) return;

    const uint8_t message_opcode = s_ws_rx.opcode;
    const size_t message_length = s_ws_rx.length;
    s_ws_rx.active = false;
    s_ws_rx.data[message_length] = '\0';
    handle_complete_ws_message(message_opcode, s_ws_rx.data,
                               message_length);
    s_ws_rx.length = 0U;
    s_ws_rx.opcode = 0U;
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
        ws_rx_reset(false);
        ESP_LOGI(TAG, "WebSocket connected");
        (void)log_memory_status("after-WebSocket-connect", false);
        queue_control_event(VOICE_CONTROL_TRANSPORT_CONNECTED, false, NULL);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ws_rx_reset(false);
        ESP_LOGW(TAG, "WebSocket disconnected; automatic reconnect in %d ms",
                 CONFIG_SMARTSCORE_DOUBAO_VOICE_RECONNECT_MS);
        queue_control_event(VOICE_CONTROL_TRANSPORT_DISCONNECTED, true,
                            "websocket-disconnected");
        break;
    case WEBSOCKET_EVENT_ERROR:
        if (data == NULL) {
            ESP_LOGE(TAG, "WebSocket error without diagnostic payload");
        } else if (data->error_handle.error_type ==
                   WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG,
                     "WebSocket transport error: tls=0x%x stack=0x%x verify=0x%x errno=%d detail=%.*s",
                     (unsigned)data->error_handle.esp_tls_last_esp_err,
                     (unsigned)data->error_handle.esp_tls_stack_err,
                     (unsigned)data->error_handle.esp_tls_cert_verify_flags,
                     data->error_handle.esp_transport_sock_errno,
                     data->data_ptr != NULL && data->data_len > 0
                         ? data->data_len : 0,
                     data->data_ptr != NULL ? (char *)data->data_ptr : "");
        } else if (data->error_handle.error_type ==
                   WEBSOCKET_ERROR_TYPE_HANDSHAKE) {
            ESP_LOGE(TAG, "WebSocket handshake error: HTTP=%d detail=%.*s",
                     data->error_handle.esp_ws_handshake_status_code,
                     data->data_ptr != NULL && data->data_len > 0
                         ? data->data_len : 0,
                     data->data_ptr != NULL ? (char *)data->data_ptr : "");
        } else {
            ESP_LOGE(TAG, "WebSocket error: type=%d detail=%.*s",
                     data->error_handle.error_type,
                     data->data_ptr != NULL && data->data_len > 0
                         ? data->data_len : 0,
                     data->data_ptr != NULL ? (char *)data->data_ptr : "");
        }
        if (data != NULL &&
            data->error_handle.error_type == WEBSOCKET_ERROR_TYPE_HANDSHAKE &&
            data->error_handle.esp_ws_handshake_status_code >= 400) {
            char reason[32];
            snprintf(reason, sizeof(reason), "websocket-http-%d",
                     data->error_handle.esp_ws_handshake_status_code);
            queue_control_event(VOICE_CONTROL_TRANSPORT_ERROR, true, reason);
        } else {
            queue_control_event(VOICE_CONTROL_TRANSPORT_ERROR, false,
                                "websocket-transport-error");
        }
        break;
    case WEBSOCKET_EVENT_DATA: {
        handle_ws_data_event(data);
        break;
    }
    case WEBSOCKET_EVENT_CLOSED:
        ws_rx_reset(false);
        ESP_LOGI(TAG, "WebSocket closed by peer");
        queue_control_event(VOICE_CONTROL_TRANSPORT_PEER_CLOSED, false,
                            "websocket-peer-closed");
        break;
    default:
        break;
    }
}

static esp_err_t start_websocket(void)
{
    int header_length = snprintf(s_ws_headers, sizeof(s_ws_headers),
                                 "X-Api-Key: %s\r\n",
                                 DOUBAO_VOICE_API_KEY);
    if (header_length <= 0 || header_length >= (int)sizeof(s_ws_headers)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_websocket_client_config_t config = {
        .uri = CONFIG_SMARTSCORE_DOUBAO_VOICE_ENDPOINT,
        .disable_auto_reconnect = false,
        .enable_close_reconnect = false,
        .task_prio = WS_TASK_PRIORITY,
        .task_name = "doubao_ws",
        .task_stack = WS_TASK_STACK_BYTES,
        .buffer_size = WS_BUFFER_BYTES,
        .headers = s_ws_headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .ping_interval_sec = 10,
        .reconnect_timeout_ms = CONFIG_SMARTSCORE_DOUBAO_VOICE_RECONNECT_MS,
        .network_timeout_ms = 10000,
    };
    s_client = esp_websocket_client_init(&config);
    if (s_client == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_websocket_register_events(
        s_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    if (err == ESP_OK) err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        return err;
    }
    ESP_LOGI(TAG, "connecting WebSocket: %s",
             CONFIG_SMARTSCORE_DOUBAO_VOICE_ENDPOINT);
    return ESP_OK;
}

static void end_session_keep_transport(void)
{
    bool should_close;
    bool close_sent = false;
    portENTER_CRITICAL(&s_state_lock);
    should_close = s_session_create_sent || s_session_ready;
    portEXIT_CRITICAL(&s_state_lock);
    if (should_close && s_client != NULL &&
        esp_websocket_client_is_connected(s_client)) {
        esp_err_t err = send_simple_event("session.close");
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "session.close failed: %s",
                     esp_err_to_name(err));
        } else {
            close_sent = true;
        }
    }
    (void)speaker_service_stream_abort();
    log_tts_ring_metrics("session-end");
    tts_pending_reset();
    portENTER_CRITICAL(&s_state_lock);
    s_armed = false;
    s_session_requested = false;
    s_session_ready = false;
    s_session_create_sent = false;
    s_input_commit_sent = false;
    s_input_commit_acknowledged = false;
    s_accept_input = false;
    s_committed = false;
    s_utterance_closed = false;
    s_cancel_response_requested = false;
    s_report_input_completed = false;
    s_response_audio_done = false;
    s_playback_seen_active = false;
    s_output_start_pending = false;
    s_output_start_command_sent = false;
    s_output_stream_started = false;
    s_output_done_pending = false;
    s_response_done_received = false;
    s_response_watchdog_reported = false;
    s_session_close_pending = close_sent;
    s_session_close_deadline_us = close_sent
                                      ? esp_timer_get_time() + 1200000ULL
                                      : 0U;
    portEXIT_CRITICAL(&s_state_lock);
    set_session_state(VOICE_SESSION_IDLE, "session-ended");
}

static void stop_websocket(void)
{
    if (s_client != NULL) {
        if (esp_websocket_client_is_connected(s_client)) {
            (void)send_simple_event("session.close");
            (void)esp_websocket_client_close(
                s_client, pdMS_TO_TICKS(1000));
        } else {
            (void)esp_websocket_client_stop(s_client);
        }
        (void)esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    ws_rx_reset(true);
    (void)speaker_service_stream_abort();
    tts_pending_reset();
    portENTER_CRITICAL(&s_state_lock);
    s_connected = false;
    s_session_ready = false;
    s_session_create_sent = false;
    s_input_commit_sent = false;
    s_input_commit_acknowledged = false;
    s_cancel_response_requested = false;
    s_accept_input = false;
    s_report_input_completed = false;
    s_response_audio_done = false;
    s_playback_seen_active = false;
    s_committed = false;
    s_utterance_closed = false;
    s_output_start_pending = false;
    s_output_start_command_sent = false;
    s_output_stream_started = false;
    s_output_done_pending = false;
    s_response_done_received = false;
    s_response_watchdog_reported = false;
    s_session_close_pending = false;
    s_session_close_deadline_us = 0;
    portEXIT_CRITICAL(&s_state_lock);
    set_session_state(VOICE_SESSION_IDLE, "transport-stopped");
}

static void process_control_event(const voice_control_event_t *event)
{
    if (event == NULL) return;
    switch (event->type) {
    case VOICE_CONTROL_TRANSPORT_CONNECTED:
        portENTER_CRITICAL(&s_state_lock);
        s_connected = true;
        s_session_ready = false;
        s_session_create_sent = false;
        s_input_commit_sent = false;
        s_input_commit_acknowledged = false;
        s_report_connected = true;
        s_connect_failures = 0;
        s_session_close_pending = false;
        portEXIT_CRITICAL(&s_state_lock);
        break;
    case VOICE_CONTROL_TRANSPORT_DISCONNECTED: {
        portENTER_CRITICAL(&s_state_lock);
        const bool active = s_session_requested;
        s_connected = false;
        s_session_ready = false;
        s_session_create_sent = false;
        s_session_close_pending = false;
        portEXIT_CRITICAL(&s_state_lock);
        if (active) request_stop(event->reason, true);
        break;
    }
    case VOICE_CONTROL_TRANSPORT_ERROR: {
        const uint32_t failures = record_connection_failure();
        ESP_LOGW(TAG, "WebSocket connection failure %lu/%u",
                 (unsigned long)failures, (unsigned)MAX_CONNECT_FAILURES);
        portENTER_CRITICAL(&s_state_lock);
        const bool active = s_session_requested;
        portEXIT_CRITICAL(&s_state_lock);
        if (event->fatal && active) {
            request_stop(event->reason, true);
        } else if (failures >= MAX_CONNECT_FAILURES && active) {
            request_stop("websocket-retries-exhausted", true);
        }
        break;
    }
    case VOICE_CONTROL_TRANSPORT_PEER_CLOSED: {
        portENTER_CRITICAL(&s_state_lock);
        const bool active = s_session_requested;
        portEXIT_CRITICAL(&s_state_lock);
        if (active) request_stop(event->reason, false);
        break;
    }
    case VOICE_CONTROL_SESSION_CREATED:
        mark_latency_once(&s_latency.session_created_us);
        portENTER_CRITICAL(&s_state_lock);
        s_session_ready = true;
        s_report_session_started = true;
        s_next_audio_send_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_state_lock);
        break;
    case VOICE_CONTROL_INPUT_COMMITTED: {
        portENTER_CRITICAL(&s_state_lock);
        s_input_commit_acknowledged = true;
        s_report_input_completed = true;
        portEXIT_CRITICAL(&s_state_lock);
        break;
    }
    case VOICE_CONTROL_ASR_STARTED: {
        mark_latency_once(&s_latency.first_asr_us);
        speaker_stream_metrics_t speaker;
        speaker_service_stream_get_metrics(&speaker);
        if (speaker.active) {
            (void)speaker_service_stream_abort();
            portENTER_CRITICAL(&s_state_lock);
            s_cancel_response_requested = true;
            portEXIT_CRITICAL(&s_state_lock);
            ESP_LOGI(TAG, "user interruption detected; playback aborted");
        }
        break;
    }
    case VOICE_CONTROL_ASR_COMPLETED: {
        mark_latency_once(&s_latency.first_asr_us);
        (void)stop_input_upload(
            event->reason[0] != '\0' ? event->reason : "asr-final", 0U);
        portENTER_CRITICAL(&s_state_lock);
        const bool committed = s_committed;
        const bool input_commit_sent = s_input_commit_sent;
        const voice_session_state_t current_state = s_session_state;
        portEXIT_CRITICAL(&s_state_lock);
        if (!input_commit_sent) {
            ESP_LOGW(TAG,
                     "ASR final arrived before client input commit; commit will be sent immediately after tail trim");
        }
        if (committed &&
            (current_state == VOICE_SESSION_LISTENING ||
             current_state == VOICE_SESSION_WAIT_RESPONSE)) {
            set_session_state(VOICE_SESSION_WAIT_RESPONSE, "asr-final");
        }
        break;
    }
    case VOICE_CONTROL_ASR_FAILED:
        request_stop(event->reason[0] != '\0'
                         ? event->reason : "asr-failed", true);
        break;
    case VOICE_CONTROL_OUTPUT_STARTED:
        mark_latency_once(&s_latency.response_audio_started_us);
        portENTER_CRITICAL(&s_state_lock);
        s_response_audio_done = false;
        s_playback_seen_active = false;
        s_output_start_pending = true;
        s_output_start_command_sent = false;
        s_output_stream_started = false;
        s_output_done_pending = false;
        portEXIT_CRITICAL(&s_state_lock);
        set_session_state(VOICE_SESSION_SPEAKING,
                          "doubao-audio-started");
        break;
    case VOICE_CONTROL_OUTPUT_DONE: {
        mark_latency_once(&s_latency.output_audio_done_us);
        portENTER_CRITICAL(&s_state_lock);
        s_output_done_pending = true;
        const bool committed = s_committed;
        portEXIT_CRITICAL(&s_state_lock);
        log_tts_ring_metrics("producer-done");
        if (committed) {
            set_session_state(VOICE_SESSION_DRAINING,
                              "doubao-audio-done");
        }
        break;
    }
    case VOICE_CONTROL_RESPONSE_DONE: {
        portENTER_CRITICAL(&s_state_lock);
        s_response_done_received = true;
        portEXIT_CRITICAL(&s_state_lock);
        break;
    }
    case VOICE_CONTROL_SESSION_CLOSED: {
        portENTER_CRITICAL(&s_state_lock);
        const bool active = s_session_requested;
        const bool normal_closing = s_session_close_pending;
        if (normal_closing) s_session_close_pending = false;
        portEXIT_CRITICAL(&s_state_lock);
        if (normal_closing) {
            ESP_LOGI(TAG, "cloud session closed; warm transport ready");
        } else if (active) {
            request_stop("session.closed", false);
        }
        break;
    }
    case VOICE_CONTROL_AUDIO_FLOW:
        ESP_LOGI(TAG,
                 "AUDIO_FLOW idx=%lu bytes=%lu gap=%lu ms total=%llu avg=%llu B/s expected=%llu B/s",
                 (unsigned long)event->audio_delta_index,
                 (unsigned long)event->decoded_pcm_bytes,
                 (unsigned long)event->audio_delta_gap_ms,
                 (unsigned long long)event->audio_pcm_total_bytes,
                 (unsigned long long)event->average_pcm_bytes_per_second,
                 (unsigned long long)event->expected_pcm_bytes_per_second);
        if (event->upstream_audio_gap) {
            ESP_LOGW(TAG,
                     "UPSTREAM_AUDIO_GAP idx=%lu gap=%lu ms avg=%llu B/s expected=%llu B/s",
                     (unsigned long)event->audio_delta_index,
                     (unsigned long)event->audio_delta_gap_ms,
                     (unsigned long long)event->average_pcm_bytes_per_second,
                     (unsigned long long)event->expected_pcm_bytes_per_second);
        }
        if (event->callback_elapsed_ms > 20U) {
            ESP_LOGW(TAG,
                     "audio WebSocket callback slow: idx=%lu elapsed=%lu ms",
                     (unsigned long)event->audio_delta_index,
                     (unsigned long)event->callback_elapsed_ms);
        }
        break;
    default:
        break;
    }
}

static esp_err_t pump_tts_pending_once(bool *made_progress)
{
    if (made_progress != NULL) *made_progress = false;
    if (s_tts_pending_lock == NULL ||
        xSemaphoreTake(s_tts_pending_lock,
                       pdMS_TO_TICKS(2)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_tts_pending_bytes == 0U) {
        xSemaphoreGive(s_tts_pending_lock);
        return ESP_OK;
    }
    const size_t requested_bytes = s_tts_pending_bytes;
    size_t accepted_samples = 0U;
    esp_err_t err = speaker_service_stream_write(
        (const int16_t *)(s_tts_pending_data + s_tts_pending_offset),
        requested_bytes / sizeof(int16_t), &accepted_samples);
    const size_t accepted_bytes = accepted_samples * sizeof(int16_t);
    if (accepted_bytes > s_tts_pending_bytes) {
        xSemaphoreGive(s_tts_pending_lock);
        return ESP_ERR_INVALID_SIZE;
    }
    s_tts_pending_offset += accepted_bytes;
    s_tts_pending_bytes -= accepted_bytes;
    if (s_tts_pending_bytes == 0U) s_tts_pending_offset = 0U;
    xSemaphoreGive(s_tts_pending_lock);

    portENTER_CRITICAL(&s_state_lock);
    if (accepted_bytes > 0U) {
        s_tts_pumped_pcm_bytes += accepted_bytes;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (accepted_bytes > 0U) {
        if (made_progress != NULL) *made_progress = true;
    }
    if (err == ESP_ERR_TIMEOUT || accepted_bytes < requested_bytes) {
        uint32_t backpressure;
        portENTER_CRITICAL(&s_state_lock);
        backpressure = ++s_tts_backpressure_events;
        portEXIT_CRITICAL(&s_state_lock);
        if (backpressure == 1U ||
            (backpressure % TTS_BACKPRESSURE_LOG_INTERVAL) == 0U) {
            ESP_LOGW(TAG,
                     "direct speaker ring backpressure=%lu pending=%u bytes",
                     (unsigned long)backpressure,
                     (unsigned)tts_pending_used());
        }
        return accepted_bytes > 0U || err == ESP_ERR_TIMEOUT ? ESP_OK : err;
    }
    return err;
}

static void worker_task(void *argument)
{
    (void)argument;

    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WORKER_TICK_MS));

        voice_control_event_t control;
        while (s_control_queue != NULL &&
               xQueueReceive(s_control_queue, &control, 0) == pdTRUE) {
            process_control_event(&control);
        }

        bool requested;
        bool network_ready;
        bool committed;
        bool connected;
        bool ready;
        bool create_sent;
        bool utterance_closed;
        bool input_commit_sent;
        bool cancel_response;
        bool stop;
        bool stop_error;
        bool report_connected_now;
        bool report_started_now;
        bool report_input_now;
        bool response_audio_done;
        bool playback_seen_active;
        bool output_start_pending;
        bool output_start_command_sent;
        bool output_stream_started;
        bool output_done_pending;
        bool session_close_pending;
        uint64_t session_close_deadline_us;
        uint64_t next_pipeline_log_us;
        char stop_reason[MAX_STOP_REASON_BYTES];
        portENTER_CRITICAL(&s_state_lock);
        network_ready = s_network_ready;
        requested = s_session_requested;
        committed = s_committed;
        connected = s_connected;
        ready = s_session_ready;
        create_sent = s_session_create_sent;
        utterance_closed = s_utterance_closed;
        input_commit_sent = s_input_commit_sent;
        cancel_response = s_cancel_response_requested;
        s_cancel_response_requested = false;
        stop = s_stop_requested;
        stop_error = s_stop_is_error;
        strlcpy(stop_reason, s_stop_reason, sizeof(stop_reason));
        report_connected_now = s_report_connected;
        s_report_connected = false;
        report_started_now = s_report_session_started;
        s_report_session_started = false;
        report_input_now = s_report_input_completed;
        s_report_input_completed = false;
        response_audio_done = s_response_audio_done;
        playback_seen_active = s_playback_seen_active;
        output_start_pending = s_output_start_pending;
        output_start_command_sent = s_output_start_command_sent;
        output_stream_started = s_output_stream_started;
        output_done_pending = s_output_done_pending;
        session_close_pending = s_session_close_pending;
        session_close_deadline_us = s_session_close_deadline_us;
        next_pipeline_log_us = s_next_pipeline_log_us;
        portEXIT_CRITICAL(&s_state_lock);

        if (report_connected_now) {
            report_event(VOICE_ASSISTANT_EVENT_CONNECTED, ESP_OK,
                         "websocket-connected");
        }
        if (report_started_now) {
            report_event(VOICE_ASSISTANT_EVENT_SESSION_STARTED, ESP_OK,
                         "session.created");
        }
        if (stop) {
            end_session_keep_transport();
            ring_reset();
            portENTER_CRITICAL(&s_state_lock);
            s_stop_requested = false;
            portEXIT_CRITICAL(&s_state_lock);
            report_event(stop_error ? VOICE_ASSISTANT_EVENT_ERROR
                                    : VOICE_ASSISTANT_EVENT_SESSION_ENDED,
                         stop_error ? ESP_FAIL : ESP_OK, stop_reason);
            continue;
        }
        if (report_input_now) {
            report_event(VOICE_ASSISTANT_EVENT_INPUT_COMPLETED, ESP_OK,
                         "single-turn-input-completed");
        }
        if (!network_ready) {
            if (s_client != NULL) {
                stop_websocket();
                ESP_LOGI(TAG, "warm WebSocket transport stopped: network offline");
            }
            continue;
        }
        if (s_client == NULL) {
            if (DOUBAO_VOICE_API_KEY[0] == '\0') {
                if (!s_api_key_missing_logged) {
                    s_api_key_missing_logged = true;
                    ESP_LOGE(TAG, "DOUBAO_VOICE_API_KEY is not configured");
                }
                continue;
            }
            const uint64_t now_us = esp_timer_get_time();
            if (now_us < s_next_transport_attempt_us) continue;
            s_next_transport_attempt_us = now_us +
                (uint64_t)CONFIG_SMARTSCORE_DOUBAO_VOICE_RECONNECT_MS * 1000ULL;
            if (!log_memory_status("before-warm-WebSocket-connect", true)) {
                continue;
            }
            esp_err_t err = start_websocket();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "warm WebSocket start failed: %s",
                         esp_err_to_name(err));
            }
            continue;
        }
        uint64_t worker_now_us = esp_timer_get_time();
        if (committed && worker_now_us >= next_pipeline_log_us) {
            speaker_stream_metrics_t speaker;
            char last_server[MAX_STOP_REASON_BYTES];
            speaker_service_stream_get_metrics(&speaker);
            portENTER_CRITICAL(&s_state_lock);
            const voice_session_state_t pipeline_state = s_session_state;
            const uint64_t received = s_tts_received_pcm_bytes;
            strlcpy(last_server, s_last_server_event,
                    sizeof(last_server));
            portEXIT_CRITICAL(&s_state_lock);
            ESP_LOGI(TAG,
                     "VOICE_STREAM: state=%s pcm_received=%llu speaker_buffered=%u played=%llu underrun=%lu last_server_event=%s",
                     session_state_name(pipeline_state),
                     (unsigned long long)received,
                     (unsigned)speaker.buffered_bytes,
                     (unsigned long long)speaker.played_bytes,
                     (unsigned long)speaker.underruns,
                     last_server);
            portENTER_CRITICAL(&s_state_lock);
            s_next_pipeline_log_us = worker_now_us +
                                     PIPELINE_LOG_INTERVAL_US;
            portEXIT_CRITICAL(&s_state_lock);
        }
        if (session_close_pending &&
            worker_now_us >= session_close_deadline_us) {
            portENTER_CRITICAL(&s_state_lock);
            s_session_close_pending = false;
            portEXIT_CRITICAL(&s_state_lock);
            session_close_pending = false;
            ESP_LOGW(TAG,
                     "previous session.close acknowledgement timed out; allowing next session");
        }
        if (requested && connected && !create_sent &&
            !session_close_pending) {
            reset_cloud_session_metrics();
            esp_err_t err = send_session_create();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "session.create failed: %s",
                         esp_err_to_name(err));
                request_stop("session-create-failed", true);
            } else {
                portENTER_CRITICAL(&s_state_lock);
                s_session_create_sent = true;
                portEXIT_CRITICAL(&s_state_lock);
            }
            continue;
        }
        if (cancel_response && ready) {
            esp_err_t err = send_simple_event("response.cancel");
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "response.cancel failed: %s",
                         esp_err_to_name(err));
            }
        }
        if (requested && ready && committed && output_start_pending &&
            !output_start_command_sent) {
            uint32_t output_sample_rate_hz;
            uint8_t output_bits;
            uint8_t output_channels;
            char output_codec[sizeof(s_output_codec)];
            portENTER_CRITICAL(&s_state_lock);
            output_sample_rate_hz = s_output_sample_rate_hz;
            output_bits = s_output_bits;
            output_channels = s_output_channels;
            strlcpy(output_codec, s_output_codec, sizeof(output_codec));
            portEXIT_CRITICAL(&s_state_lock);
            ESP_LOGI(TAG,
                     "speaker applying negotiated PCM: codec=%s rate=%lu bits=%u channels=%u",
                     output_codec, (unsigned long)output_sample_rate_hz,
                     (unsigned)output_bits, (unsigned)output_channels);
            esp_err_t err = speaker_service_stream_start(
                output_sample_rate_hz);
            if (err != ESP_OK) {
                ESP_LOGE(TAG,
                         "speaker stream prepare failed at %lu Hz: %s",
                         (unsigned long)output_sample_rate_hz,
                         esp_err_to_name(err));
                request_stop("speaker-start-failed", true);
                continue;
            }
            portENTER_CRITICAL(&s_state_lock);
            s_output_start_command_sent = true;
            portEXIT_CRITICAL(&s_state_lock);
            output_start_command_sent = true;
        }
        if (requested && ready && committed && output_start_pending &&
            output_start_command_sent && !output_stream_started) {
            speaker_stream_metrics_t speaker;
            speaker_service_stream_get_metrics(&speaker);
            if (speaker.active) {
                portENTER_CRITICAL(&s_state_lock);
                s_output_stream_started = true;
                s_output_start_pending = false;
                portEXIT_CRITICAL(&s_state_lock);
                output_stream_started = true;
                output_start_pending = false;
            }
        }
        if (output_stream_started && output_start_command_sent) {
            set_session_state(output_done_pending
                                  ? VOICE_SESSION_DRAINING
                                  : VOICE_SESSION_SPEAKING,
                              output_done_pending
                                  ? "speaker-started-after-tts-done"
                                  : "speaker-stream-started");
            portENTER_CRITICAL(&s_state_lock);
            s_output_start_command_sent = false;
            portEXIT_CRITICAL(&s_state_lock);
            output_start_command_sent = false;
            ESP_LOGI(TAG, "AI PCM released to 256KB speaker ring");
        }
        if (requested && ready && output_stream_started) {
            for (unsigned transfer = 0;
                 transfer < MAX_TTS_TRANSFERS_PER_TICK;
                 ++transfer) {
                bool progress = false;
                esp_err_t err = pump_tts_pending_once(&progress);
                if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
                    ESP_LOGE(TAG, "speaker PCM pump failed: %s",
                             esp_err_to_name(err));
                    request_stop("speaker-write-failed", true);
                    break;
                }
                if (!progress) break;
            }
            speaker_stream_metrics_t playback_metrics;
            speaker_service_stream_get_metrics(&playback_metrics);
            if (playback_metrics.output_started) {
                mark_latency_once(
                    &s_latency.first_speaker_playback_us);
            }
        }
        if (requested && ready && committed && input_commit_sent &&
            !output_done_pending &&
            !response_audio_done) {
            char last_server[MAX_STOP_REASON_BYTES];
            char last_client[MAX_STOP_REASON_BYTES];
            uint64_t last_any_business_rx_ms;
            uint64_t last_audio_delta_ms;
            uint64_t received_pcm;
            voice_session_state_t watchdog_state;
            bool already_reported;
            portENTER_CRITICAL(&s_state_lock);
            last_any_business_rx_ms = s_last_any_business_rx_ms;
            last_audio_delta_ms = s_last_audio_delta_ms;
            received_pcm = s_tts_received_pcm_bytes;
            watchdog_state = s_session_state;
            already_reported = s_response_watchdog_reported;
            strlcpy(last_server, s_last_server_event,
                    sizeof(last_server));
            strlcpy(last_client, s_last_client_event,
                    sizeof(last_client));
            portEXIT_CRITICAL(&s_state_lock);
            const uint64_t watchdog_now_ms =
                esp_timer_get_time() / 1000ULL;
            if (!already_reported && last_any_business_rx_ms > 0U &&
                watchdog_state == VOICE_SESSION_WAIT_RESPONSE &&
                watchdog_now_ms - last_any_business_rx_ms >=
                    RESPONSE_WATCHDOG_DIAGNOSTIC_MS) {
                const uint64_t silent_ms =
                    watchdog_now_ms - last_any_business_rx_ms;
                const uint64_t audio_silent_ms = last_audio_delta_ms > 0U
                    ? watchdog_now_ms - last_audio_delta_ms : 0U;
                portENTER_CRITICAL(&s_state_lock);
                s_response_watchdog_reported = true;
                portEXIT_CRITICAL(&s_state_lock);
                ESP_LOGW(TAG,
                         "response watchdog diagnostic only: last_server_event=%s last_client_event=%s state=%s received_pcm=%llu business_silence=%llu ms audio_silence=%llu ms; session remains open",
                         last_server, last_client,
                         session_state_name(watchdog_state),
                         (unsigned long long)received_pcm,
                         (unsigned long long)silent_ms,
                         (unsigned long long)audio_silent_ms);
            }
        }

        if (requested && ready && output_stream_started &&
            output_done_pending &&
            tts_pending_used() == 0U &&
            !response_audio_done) {
            esp_err_t err = speaker_service_stream_finish();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "speaker stream finish failed: %s",
                         esp_err_to_name(err));
                request_stop("speaker-finish-failed", true);
                continue;
            }
            portENTER_CRITICAL(&s_state_lock);
            s_response_audio_done = true;
            s_output_done_pending = false;
            portEXIT_CRITICAL(&s_state_lock);
            response_audio_done = true;
            output_done_pending = false;
            set_session_state(VOICE_SESSION_DRAINING,
                              "all-tts-queued-to-speaker");
            ESP_LOGI(TAG, "all TTS pumped; draining speaker ring and I2S");
        }
        if (requested && ready) {
            speaker_status_t status;
            speaker_service_get_status(&status);
            if (status.state == SPEAKER_STATE_STREAM &&
                !playback_seen_active) {
                portENTER_CRITICAL(&s_state_lock);
                s_playback_seen_active = true;
                portEXIT_CRITICAL(&s_state_lock);
                playback_seen_active = true;
            }
            if (response_audio_done && playback_seen_active &&
                status.state != SPEAKER_STATE_STREAM) {
                if (status.state == SPEAKER_STATE_ERROR) {
                    request_stop("speaker-playback-error", true);
                } else {
                    ESP_LOGI(TAG,
                             "single-turn answer playback drained; closing session");
                    log_latency_summary();
                    request_stop("single-turn-complete", false);
                }
                continue;
            }
        }
        if (!requested || !ready) continue;

        const unsigned send_budget = utterance_closed
                                         ? MAX_INPUT_FLUSH_CHUNKS_PER_TICK
                                         : 1U;
        for (unsigned sent_this_tick = 0U;
             sent_this_tick < send_budget;
             ++sent_this_tick) {
            const size_t pending = ring_used();
            const size_t send_chunk_bytes = utterance_closed
                                                ? INPUT_FAST_FLUSH_CHUNK_BYTES
                                                : INPUT_CHUNK_BYTES;
            const bool full_chunk = pending >= send_chunk_bytes;
            const bool final_tail = utterance_closed && pending > 0U;
            const uint64_t now_us = esp_timer_get_time();
            if ((!full_chunk && !final_tail) ||
                (!utterance_closed && now_us < s_next_audio_send_us)) {
                break;
            }
            size_t bytes = ring_read(s_input_send_pcm,
                                     send_chunk_bytes);
            if (bytes == 0U) break;
            esp_err_t err = send_audio_chunk(s_input_send_pcm, bytes);
            if (err != ESP_OK) {
                ring_unread(bytes);
                ESP_LOGW(TAG, "Doubao audio send failed at chunk=%lu: %s",
                         (unsigned long)s_audio_sent_chunks,
                         esp_err_to_name(err));
                s_next_audio_send_us = now_us + INPUT_CHUNK_DURATION_US;
                break;
            }
            ++s_audio_sent_chunks;
            portENTER_CRITICAL(&s_state_lock);
            s_input_uploaded_pcm_bytes += bytes;
            portEXIT_CRITICAL(&s_state_lock);
            s_next_audio_send_us = utterance_closed
                                       ? now_us
                                       : now_us + INPUT_CHUNK_DURATION_US;
        }

        if (utterance_closed && !input_commit_sent && ring_used() == 0U) {
            esp_err_t err = send_simple_event("input_audio_buffer.commit");
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "input_audio_buffer.commit failed: %s",
                         esp_err_to_name(err));
                request_stop("input-commit-failed", true);
                continue;
            }
            uint64_t uploaded_pcm_bytes;
            portENTER_CRITICAL(&s_state_lock);
            s_input_commit_sent = true;
            uploaded_pcm_bytes = s_input_uploaded_pcm_bytes;
            const voice_session_state_t state_before_commit =
                s_session_state;
            portEXIT_CRITICAL(&s_state_lock);
            input_commit_sent = true;
            mark_latency_once(&s_latency.input_commit_us);
            if (state_before_commit == VOICE_SESSION_LISTENING ||
                state_before_commit == VOICE_SESSION_WAIT_RESPONSE) {
                set_session_state(VOICE_SESSION_WAIT_RESPONSE,
                                  "client-input-commit");
            }
            ESP_LOGI(TAG,
                     "all buffered user PCM uploaded; official input commit sent total=%llu bytes",
                     (unsigned long long)uploaded_pcm_bytes);
        }
    }
}

static void cleanup_init_allocations(void)
{
    if (s_control_queue != NULL) vQueueDelete(s_control_queue);
    if (s_tts_pending_lock != NULL) vSemaphoreDelete(s_tts_pending_lock);
    if (s_input.lock != NULL) vSemaphoreDelete(s_input.lock);
    free(s_tts_pending_data);
    free(s_input_send_pcm);
    free(s_input_send_base64);
    free(s_input_send_json);
    free(s_input.data);
    s_control_queue = NULL;
    s_tts_pending_lock = NULL;
    s_tts_pending_data = NULL;
    s_input_send_pcm = NULL;
    s_input_send_base64 = NULL;
    s_input_send_json = NULL;
    memset(&s_input, 0, sizeof(s_input));
}

esp_err_t voice_assistant_init(voice_assistant_event_handler_t handler,
                               void *context)
{
#if !CONFIG_SMARTSCORE_DOUBAO_VOICE_ASSISTANT
    (void)handler;
    (void)context;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_initialized) {
        s_event_handler = handler;
        s_event_context = context;
        return ESP_OK;
    }
    const size_t capacity =
        (size_t)CONFIG_SMARTSCORE_DOUBAO_VOICE_INPUT_BUFFER_SECONDS *
        INPUT_SAMPLE_RATE_HZ * sizeof(int16_t);
    s_input.data = heap_caps_malloc(capacity,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_input.data == NULL) {
        s_input.data = heap_caps_malloc(capacity, MALLOC_CAP_8BIT);
    }
    if (s_input.data == NULL) {
        ESP_LOGE(TAG, "init failed: input PCM allocation=%u bytes",
                 (unsigned)capacity);
        return ESP_ERR_NO_MEM;
    }
    s_input.capacity = capacity;
    s_input.lock = xSemaphoreCreateMutex();
    if (s_input.lock == NULL) {
        ESP_LOGE(TAG, "init failed: input PCM mutex allocation");
        free(s_input.data);
        memset(&s_input, 0, sizeof(s_input));
        return ESP_ERR_NO_MEM;
    }
    s_tts_pending_data = heap_caps_malloc(
        TTS_PENDING_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_tts_pending_lock = xSemaphoreCreateMutex();
    s_control_queue = xQueueCreate(CONTROL_QUEUE_LENGTH,
                                   sizeof(voice_control_event_t));
    s_input_send_pcm = heap_caps_malloc(
        INPUT_FAST_FLUSH_CHUNK_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_input_send_base64 = heap_caps_malloc(
        INPUT_BASE64_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_input_send_json = heap_caps_malloc(
        INPUT_JSON_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_tts_pending_data == NULL || s_tts_pending_lock == NULL ||
        s_control_queue == NULL || s_input_send_pcm == NULL ||
        s_input_send_base64 == NULL || s_input_send_json == NULL) {
        ESP_LOGE(TAG,
                 "init failed: direct PCM/scratch allocation; pending=%u free-PSRAM=%u largest-PSRAM=%u",
                 (unsigned)TTS_PENDING_BUFFER_BYTES,
                 (unsigned)heap_caps_get_free_size(
                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(
                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        cleanup_init_allocations();
        return ESP_ERR_NO_MEM;
    }
    s_event_handler = handler;
    s_event_context = context;
    s_worker_stack = heap_caps_calloc(
        1U, WORKER_TASK_STACK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_worker_stack == NULL) {
        ESP_LOGE(TAG, "init failed: PSRAM worker stack allocation=%u bytes",
                 (unsigned)WORKER_TASK_STACK_BYTES);
        cleanup_init_allocations();
        return ESP_ERR_NO_MEM;
    }
    s_worker_task = xTaskCreateStatic(
        worker_task, "voice_assistant", WORKER_TASK_STACK_BYTES, NULL,
        WORKER_TASK_PRIORITY, s_worker_stack, &s_worker_task_control);
    if (s_worker_task == NULL) {
        ESP_LOGE(TAG, "init failed: worker task creation");
        free(s_worker_stack);
        s_worker_stack = NULL;
        cleanup_init_allocations();
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG,
             "initialized: input=%u, speaker-direct PCM, pending=%u, worker-stack=%u bytes; API key=%s",
             (unsigned)capacity, (unsigned)TTS_PENDING_BUFFER_BYTES,
             (unsigned)WORKER_TASK_STACK_BYTES,
             DOUBAO_VOICE_API_KEY[0] != '\0' ? "configured" : "missing");
    (void)log_memory_status("after-voice-init", false);
    return ESP_OK;
#endif
}

void voice_assistant_set_context_provider(
    voice_assistant_context_provider_t provider,
    void *context)
{
    s_context_provider = provider;
    s_context_provider_context = context;
}

void voice_assistant_set_network_ready(bool ready)
{
    if (!s_initialized) return;
    bool changed;
    portENTER_CRITICAL(&s_state_lock);
    changed = s_network_ready != ready;
    s_network_ready = ready;
    if (!ready && s_session_requested) {
        s_stop_requested = true;
        s_stop_is_error = true;
        strlcpy(s_stop_reason, "network-offline", sizeof(s_stop_reason));
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (changed) {
        ESP_LOGI(TAG, "network %s; warm WebSocket transport %s",
                 ready ? "ready" : "offline",
                 ready ? "requested" : "will stop");
        notify_worker();
    }
}

esp_err_t voice_assistant_arm(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    ring_reset();
    tts_pending_reset();
    const uint64_t wake_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    memset(&s_latency, 0, sizeof(s_latency));
    s_latency.wake_us = wake_us;
    s_armed = true;
    /* WAKE is local-candidate only.  The warm transport stays connected,
     * but no Doubao conversation exists until S3 sends AI_BEGIN. */
    s_session_requested = false;
    s_accept_input = true;
    s_committed = false;
    s_utterance_closed = false;
    s_input_commit_sent = false;
    s_input_commit_acknowledged = false;
    s_stop_requested = false;
    s_report_input_completed = false;
    s_response_audio_done = false;
    s_playback_seen_active = false;
    s_input_capacity_exhausted = false;
    s_next_audio_send_us = esp_timer_get_time();
    s_output_start_pending = false;
    s_output_start_command_sent = false;
    s_output_stream_started = false;
    s_output_done_pending = false;
    s_response_done_received = false;
    s_response_watchdog_reported = false;
    s_audio_sent_chunks = 0;
    s_input_overruns = 0;
    s_input_total_pcm_bytes = 0U;
    s_input_uploaded_pcm_bytes = 0U;
    s_tts_received_pcm_bytes = 0U;
    s_tts_pumped_pcm_bytes = 0U;
    s_tts_backpressure_events = 0U;
    s_tts_overflow_events = 0U;
    s_connect_failures = 0;
    s_last_any_business_rx_ms = wake_us / 1000ULL;
    s_last_audio_delta_ms = 0U;
    s_audio_flow_last_delta_us = 0U;
    s_audio_flow_start_us = 0U;
    s_audio_flow_total_pcm_bytes = 0U;
    s_audio_delta_index = 0U;
    s_output_sample_rate_hz = OUTPUT_SAMPLE_RATE_HZ;
    s_output_bits = 16U;
    s_output_channels = 1U;
    strlcpy(s_output_codec, "pcm_s16le", sizeof(s_output_codec));
    strlcpy(s_last_client_event, "none", sizeof(s_last_client_event));
    strlcpy(s_last_server_event, "none", sizeof(s_last_server_event));
    s_next_pipeline_log_us = esp_timer_get_time() +
                             PIPELINE_LOG_INTERVAL_US;
    portEXIT_CRITICAL(&s_state_lock);
    set_session_state(VOICE_SESSION_CANDIDATE, "wake-local-candidate");
    ESP_LOGI(TAG,
             "WAKE: buffering local candidate only; cloud session not created");
    notify_worker();
    return ESP_OK;
}

esp_err_t voice_assistant_begin(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (DOUBAO_VOICE_API_KEY[0] == '\0') {
        ESP_LOGE(TAG, "DOUBAO_VOICE_API_KEY is not configured");
        return ESP_ERR_INVALID_STATE;
    }
    speaker_status_t speaker;
    speaker_service_get_status(&speaker);
    if (!speaker.hardware_output_enabled) {
        ESP_LOGE(TAG, "speaker hardware is disabled; AI conversation rejected");
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_state_lock);
    bool armed = s_armed;
    portEXIT_CRITICAL(&s_state_lock);
    if (!armed) return ESP_ERR_INVALID_STATE;
    mark_latency_once(&s_latency.ai_begin_us);

    /* No candidate PCM has been uploaded before AI_BEGIN, so preserve the
     * complete locally buffered utterance and only stop accepting new tail. */
    (void)stop_input_upload("s3-ai-begin", s_input.capacity);
    portENTER_CRITICAL(&s_state_lock);
    if (!s_armed) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_session_requested = true;
    s_committed = true;
    portEXIT_CRITICAL(&s_state_lock);
    tts_pending_reset();
    set_session_state(VOICE_SESSION_LISTENING,
                      "AI_BEGIN-open-question");
    ESP_LOGI(TAG,
             "committing one-turn AI response; captured=%u queued=%u bytes",
             (unsigned)ring_captured(), (unsigned)ring_used());
    notify_worker();
    return ESP_OK;
}

void voice_assistant_cancel_candidate(void)
{
    if (!s_initialized) return;
    (void)stop_input_upload("local-command", 0U);
    portENTER_CRITICAL(&s_state_lock);
    bool connected = s_session_requested;
    s_armed = false;
    s_session_requested = false;
    s_accept_input = false;
    s_committed = false;
    s_utterance_closed = true;
    s_input_commit_sent = false;
    s_input_commit_acknowledged = false;
    s_report_input_completed = false;
    s_response_audio_done = false;
    s_playback_seen_active = false;
    s_response_done_received = false;
    s_response_watchdog_reported = false;
    if (connected) {
        s_stop_requested = true;
        s_stop_is_error = false;
        strlcpy(s_stop_reason, "local-command", sizeof(s_stop_reason));
    }
    portEXIT_CRITICAL(&s_state_lock);
    set_session_state(VOICE_SESSION_IDLE, "local-command");
    ring_reset();
    notify_worker();
    ESP_LOGI(TAG, "candidate AI audio discarded before cloud session");
}

esp_err_t voice_assistant_push_audio(const int16_t *pcm,
                                     size_t sample_count,
                                     uint16_t sequence)
{
    if (!s_initialized || pcm == NULL || sample_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_lock);
    bool accept = s_accept_input &&
                  (s_armed || s_session_requested);
    portEXIT_CRITICAL(&s_state_lock);
    if (!accept) return ESP_ERR_INVALID_STATE;
    const size_t bytes = sample_count * sizeof(*pcm);
    if (bytes > s_input.capacity) return ESP_ERR_INVALID_SIZE;
    if (xSemaphoreTake(s_input.lock, pdMS_TO_TICKS(2)) != pdTRUE) {
        ESP_LOGW(TAG, "PCM ring busy; UART seq=%u not queued",
                 (unsigned)sequence);
        return ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&s_state_lock);
    accept = s_accept_input &&
             (s_armed || s_session_requested);
    portEXIT_CRITICAL(&s_state_lock);
    bool stored = accept
                      ? ring_write_locked((const uint8_t *)pcm, bytes)
                      : false;
    if (stored) {
        portENTER_CRITICAL(&s_state_lock);
        s_input_total_pcm_bytes += bytes;
        portEXIT_CRITICAL(&s_state_lock);
    }
    xSemaphoreGive(s_input.lock);
    if (accept) notify_worker();
    /* Capacity exhaustion is explicit in logs; ACK the UART frame so the
     * reliable link does not retry an audio tail that cannot fit. */
    return accept && stored ? ESP_OK
                            : (accept ? ESP_OK : ESP_ERR_INVALID_STATE);
}
