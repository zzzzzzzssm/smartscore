#include "voice_ai_stream.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "voice_ima_adpcm.h"
#include "voice_link_protocol.h"
#include "voice_uart_link.h"

#define AI_PCM_CHUNK_SAMPLES 320U
#define AI_ADPCM_MAX_BYTES (6U + AI_PCM_CHUNK_SAMPLES / 2U)
#define AI_TX_QUEUE_LENGTH 32U
#define AI_REPLAY_CACHE_LENGTH 24U
#define AI_TX_TASK_STACK_BYTES 4096U
#define AI_TX_TASK_PRIORITY 5U
#define AI_TX_LOG_INTERVAL_FRAMES 50U
#define AI_TAIL_ACK_WAIT_MS 1000U

typedef struct {
    uint32_t generation;
    bool speech_end;
    uint16_t sequence;
    uint16_t payload_length;
    uint8_t payload[AI_ADPCM_MAX_BYTES];
} ai_audio_frame_t;

typedef struct {
    bool valid;
    ai_audio_frame_t frame;
} ai_cache_entry_t;

static const char *TAG = "voice_ai_stream";
static QueueHandle_t s_tx_queue;
static TaskHandle_t s_tx_task;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_active;
static bool s_accepting_pcm;
static uint32_t s_generation;
static uint16_t s_next_sequence;
static uint16_t s_last_sent_sequence;
static bool s_have_sent;
static uint16_t s_last_acked_sequence;
static bool s_have_ack;
static bool s_replay_pending;
static uint16_t s_replay_sequence;
static uint32_t s_dropped_frames;
static ai_cache_entry_t s_cache[AI_REPLAY_CACHE_LENGTH];

static bool sequence_before(uint16_t left, uint16_t right)
{
    return (int16_t)(left - right) < 0;
}

static bool current_generation(uint32_t generation)
{
    bool current;
    portENTER_CRITICAL(&s_lock);
    current = s_active && generation == s_generation;
    portEXIT_CRITICAL(&s_lock);
    return current;
}

static void cache_frame(const ai_audio_frame_t *frame)
{
    portENTER_CRITICAL(&s_lock);
    ai_cache_entry_t *entry =
        &s_cache[frame->sequence % AI_REPLAY_CACHE_LENGTH];
    entry->valid = true;
    entry->frame = *frame;
    s_last_sent_sequence = frame->sequence;
    s_have_sent = true;
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t transmit_frame(const ai_audio_frame_t *frame,
                                bool replay)
{
    if (frame->speech_end) {
        return replay ? ESP_ERR_INVALID_ARG
                      : voice_uart_link_send_ai_speech_end();
    }
    uint8_t wire[VOICE_LINK_WIRE_OVERHEAD_BYTES + AI_ADPCM_MAX_BYTES];
    size_t wire_length = 0;
    esp_err_t err = voice_link_encode_packet(
        VOICE_LINK_PACKET_AUDIO,
        replay ? 1U : 0U,
        frame->sequence,
        frame->payload,
        frame->payload_length,
        wire,
        sizeof(wire),
        &wire_length);
    if (err == ESP_OK) {
        /* Keep attempted frames available until the peer has advanced. */
        if (!replay) cache_frame(frame);
        err = voice_uart_link_write_binary(wire, wire_length);
    }
    return err;
}

static void replay_requested_frames(void)
{
    uint16_t first;
    uint16_t last;
    uint32_t generation;
    bool pending;

    portENTER_CRITICAL(&s_lock);
    pending = s_active && s_replay_pending && s_have_sent;
    first = s_replay_sequence;
    last = s_last_sent_sequence;
    generation = s_generation;
    s_replay_pending = false;
    portEXIT_CRITICAL(&s_lock);
    if (!pending) return;

    uint16_t sequence = first;
    unsigned replayed = 0;
    while (!sequence_before(last, sequence) &&
           replayed < AI_REPLAY_CACHE_LENGTH &&
           current_generation(generation)) {
        ai_audio_frame_t frame;
        bool found = false;
        portENTER_CRITICAL(&s_lock);
        const ai_cache_entry_t *entry =
            &s_cache[sequence % AI_REPLAY_CACHE_LENGTH];
        if (entry->valid && entry->frame.generation == generation &&
            entry->frame.sequence == sequence) {
            frame = entry->frame;
            found = true;
        }
        portEXIT_CRITICAL(&s_lock);
        if (!found) {
            ESP_LOGE(TAG, "cannot replay expired UART audio seq=%u",
                     (unsigned)sequence);
            return;
        }
        esp_err_t err = transmit_frame(&frame, true);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "UART audio replay seq=%u failed: %s",
                     (unsigned)sequence, esp_err_to_name(err));
            return;
        }
        ++sequence;
        ++replayed;
    }
    ESP_LOGW(TAG, "UART audio replayed from seq=%u, frames=%u",
             (unsigned)first, replayed);
}

static bool wait_for_tail_ack(uint32_t generation)
{
    const TickType_t deadline = xTaskGetTickCount() +
                                pdMS_TO_TICKS(AI_TAIL_ACK_WAIT_MS);
    while (current_generation(generation)) {
        bool complete;
        uint16_t target;
        portENTER_CRITICAL(&s_lock);
        target = s_last_sent_sequence;
        complete = !s_have_sent ||
                   (s_have_ack &&
                    !sequence_before(s_last_acked_sequence, target));
        portEXIT_CRITICAL(&s_lock);
        if (complete) return true;
        replay_requested_frames();
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) break;
        vTaskDelay(1);
    }
    return false;
}

static void ai_tx_task(void *argument)
{
    (void)argument;
    ai_audio_frame_t frame;
    uint32_t sent_frames = 0;

    while (true) {
        replay_requested_frames();
        if (xQueueReceive(s_tx_queue, &frame,
                          pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (!current_generation(frame.generation)) continue;

        if (frame.speech_end) {
            const bool tail_acked = wait_for_tail_ack(frame.generation);
            if (!current_generation(frame.generation)) continue;
            if (!tail_acked) {
                ESP_LOGW(TAG,
                         "tail ACK timeout before SPEECH_END; sending ordered marker to avoid deadlock");
            }
        }
        if (!current_generation(frame.generation)) continue;

        esp_err_t err = transmit_frame(&frame, false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "UART audio send seq=%u failed: %s",
                     (unsigned)frame.sequence, esp_err_to_name(err));
            continue;
        }
        if (frame.speech_end) {
            ESP_LOGI(TAG, "ordered SPEECH_END sent after queued PCM");
            continue;
        }
        ++sent_frames;
        if ((sent_frames % AI_TX_LOG_INTERVAL_FRAMES) == 0U) {
            ESP_LOGI(TAG, "UART audio sent: seq=%u queued=%u",
                     (unsigned)frame.sequence,
                     (unsigned)uxQueueMessagesWaiting(s_tx_queue));
        }
    }
}

esp_err_t voice_ai_stream_init(void)
{
    if (s_tx_queue != NULL) return ESP_OK;
    s_tx_queue = xQueueCreate(AI_TX_QUEUE_LENGTH,
                              sizeof(ai_audio_frame_t));
    if (s_tx_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(ai_tx_task, "voice_ai_tx",
                    AI_TX_TASK_STACK_BYTES, NULL,
                    AI_TX_TASK_PRIORITY, &s_tx_task) != pdPASS) {
        vQueueDelete(s_tx_queue);
        s_tx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "AI audio bridge ready: IMA-ADPCM, 16 kHz mono");
    return ESP_OK;
}

void voice_ai_stream_start_session(void)
{
    if (s_tx_queue == NULL) return;

    portENTER_CRITICAL(&s_lock);
    s_active = false;
    ++s_generation;
    portEXIT_CRITICAL(&s_lock);

    xQueueReset(s_tx_queue);
    memset(s_cache, 0, sizeof(s_cache));

    portENTER_CRITICAL(&s_lock);
    s_next_sequence = 0;
    s_have_sent = false;
    s_have_ack = false;
    s_replay_pending = false;
    s_dropped_frames = 0;
    s_active = true;
    s_accepting_pcm = true;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "wake captured; streaming candidate PCM to P4");
}

void voice_ai_stream_stop_session(void)
{
    if (s_tx_queue == NULL) return;
    portENTER_CRITICAL(&s_lock);
    s_active = false;
    s_accepting_pcm = false;
    ++s_generation;
    s_replay_pending = false;
    portEXIT_CRITICAL(&s_lock);
    xQueueReset(s_tx_queue);
    ESP_LOGI(TAG, "AI audio bridge stopped");
}

bool voice_ai_stream_is_active(void)
{
    bool active;
    portENTER_CRITICAL(&s_lock);
    active = s_active;
    portEXIT_CRITICAL(&s_lock);
    return active;
}

esp_err_t voice_ai_stream_push_pcm(const int16_t *pcm, size_t sample_count)
{
    if (pcm == NULL || sample_count == 0U) return ESP_ERR_INVALID_ARG;
    if (s_tx_queue == NULL) return ESP_ERR_INVALID_STATE;

    size_t offset = 0;
    while (offset < sample_count) {
        ai_audio_frame_t frame = {0};
        size_t chunk = sample_count - offset;
        if (chunk > AI_PCM_CHUNK_SAMPLES) chunk = AI_PCM_CHUNK_SAMPLES;

        portENTER_CRITICAL(&s_lock);
        if (!s_active || !s_accepting_pcm) {
            portEXIT_CRITICAL(&s_lock);
            return ESP_ERR_INVALID_STATE;
        }
        frame.generation = s_generation;
        frame.sequence = s_next_sequence;
        portEXIT_CRITICAL(&s_lock);

        size_t encoded_length = 0;
        esp_err_t err = voice_ima_adpcm_encode(
            pcm + offset, chunk,
            frame.payload, sizeof(frame.payload), &encoded_length);
        if (err != ESP_OK) return err;
        frame.payload_length = (uint16_t)encoded_length;

        if (xQueueSend(s_tx_queue, &frame, 0) != pdTRUE) {
            portENTER_CRITICAL(&s_lock);
            ++s_dropped_frames;
            uint32_t dropped = s_dropped_frames;
            portEXIT_CRITICAL(&s_lock);
            if (dropped == 1U || (dropped % 50U) == 0U) {
                ESP_LOGE(TAG, "UART audio queue full; dropped=%lu",
                         (unsigned long)dropped);
            }
            return ESP_ERR_TIMEOUT;
        }
        portENTER_CRITICAL(&s_lock);
        if (s_active && frame.generation == s_generation &&
            frame.sequence == s_next_sequence) {
            ++s_next_sequence;
        }
        portEXIT_CRITICAL(&s_lock);
        offset += chunk;
    }
    return ESP_OK;
}

esp_err_t voice_ai_stream_finish_utterance(void)
{
    if (s_tx_queue == NULL) return ESP_ERR_INVALID_STATE;
    ai_audio_frame_t marker = {.speech_end = true};
    portENTER_CRITICAL(&s_lock);
    if (!s_active || !s_accepting_pcm) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    marker.generation = s_generation;
    s_accepting_pcm = false;
    portEXIT_CRITICAL(&s_lock);

    if (xQueueSend(s_tx_queue, &marker, pdMS_TO_TICKS(100)) != pdTRUE) {
        portENTER_CRITICAL(&s_lock);
        if (s_active && marker.generation == s_generation) {
            s_accepting_pcm = true;
        }
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void voice_ai_stream_handle_feedback(uint8_t packet_type,
                                     uint16_t sequence)
{
    if (packet_type == VOICE_LINK_PACKET_ACK) {
        portENTER_CRITICAL(&s_lock);
        if (s_active &&
            (!s_have_ack || !sequence_before(sequence,
                                              s_last_acked_sequence))) {
            s_last_acked_sequence = sequence;
            s_have_ack = true;
        }
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    if (packet_type != VOICE_LINK_PACKET_NACK) return;

    portENTER_CRITICAL(&s_lock);
    if (s_active) {
        s_replay_sequence = sequence;
        s_replay_pending = true;
    }
    portEXIT_CRITICAL(&s_lock);
}
