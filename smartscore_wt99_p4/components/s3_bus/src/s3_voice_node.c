#include "s3_devices.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_wt99_pins.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "s3_voice_link_protocol.h"
#include "speaker_service.h"

#define VOICE_RX_BUFFER_BYTES (16U * 1024U)
#define VOICE_TX_BUFFER_BYTES 4096U
#define VOICE_RX_TASK_STACK_BYTES 10240U
#define VOICE_PCM_TASK_STACK_BYTES 6144U
#define VOICE_RX_TASK_PRIORITY 7U
#define VOICE_PCM_TASK_PRIORITY 6U
#define VOICE_COMMAND_ID_MIN 1U
#define VOICE_COMMAND_ID_MAX 7U
#define VOICE_PCM_QUEUE_LENGTH 48U
#define VOICE_FLOW_HIGH_BYTES (224U * 1024U)
#define VOICE_FLOW_LOW_BYTES (112U * 1024U)
#define VOICE_SUMMARY_INTERVAL_US 1000000ULL
#define VOICE_FINISH_RETRY_INTERVAL_US 100000ULL

typedef struct {
    uint16_t length;
    uint8_t data[S3_VOICE_LINK_PCM_PAYLOAD_BYTES];
} voice_pcm_frame_t;

static const char *TAG = "S3_VOICE";
static bool s_initialized;
static s3_voice_event_handler_t s_handler;
static void *s_handler_context;
static SemaphoreHandle_t s_tx_lock;
static QueueHandle_t s_pcm_queue;
static volatile bool s_stream_active;
static volatile bool s_stream_done;
static volatile bool s_stream_finish_queued;
static volatile bool s_flow_off;
static volatile uint32_t s_expected_sequence;
static volatile bool s_have_sequence;
static uint32_t s_tx_sequence;
static uint64_t s_uart_pcm_received;
static uint32_t s_crc_errors;
static uint32_t s_seq_gaps;
static uint32_t s_uart_overflows;
static uint32_t s_queue_overflows;
static uint64_t s_next_summary_us;
static char s_text_buffer[S3_VOICE_LINK_MAX_PAYLOAD_BYTES + 1U];
static uint8_t *s_tx_wire;

static esp_err_t write_locked(const void *data, size_t length)
{
    if (!s_initialized || s_tx_lock == NULL) return ESP_ERR_INVALID_STATE;
    if (data == NULL || length == 0U) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(250)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    int written = uart_write_bytes((uart_port_t)BOARD_WT99_VOICE_UART_PORT,
                                   data, length);
    xSemaphoreGive(s_tx_lock);
    return written == (int)length ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_message(uint8_t type,
                              const void *payload,
                              uint16_t payload_length)
{
    if ((payload_length > 0U && payload == NULL) ||
        payload_length > S3_VOICE_LINK_MAX_PAYLOAD_BYTES)
        return ESP_ERR_INVALID_ARG;
    if (s_tx_wire == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(250)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    size_t wire_length = 0U;
    esp_err_t err = s3_voice_link_encode_packet(
        type, 0U, s_tx_sequence, payload, payload_length,
        s_tx_wire,
        S3_VOICE_LINK_WIRE_OVERHEAD_BYTES + S3_VOICE_LINK_MAX_PAYLOAD_BYTES,
        &wire_length);
    if (err == ESP_OK) {
        int written = uart_write_bytes(
            (uart_port_t)BOARD_WT99_VOICE_UART_PORT,
            s_tx_wire, wire_length);
        err = written == (int)wire_length ? ESP_OK : ESP_FAIL;
        if (err == ESP_OK) ++s_tx_sequence;
    }
    xSemaphoreGive(s_tx_lock);
    return err;
}

static void dispatch_event(s3_voice_event_type_t type,
                           uint8_t command_id,
                           uint8_t ai_state,
                           uint32_t sample_rate,
                           const char *text)
{
    if (s_handler == NULL) return;
    const s3_voice_event_t event = {
        .type = type,
        .command_id = command_id,
        .ai_state = ai_state,
        .sample_rate_hz = sample_rate,
        .text = text,
    };
    s_handler(&event, s_handler_context);
}

static void reset_pcm_queue(void)
{
    voice_pcm_frame_t frame;
    while (xQueueReceive(s_pcm_queue, &frame, 0) == pdTRUE) {
    }
}

static void handle_packet(const s3_voice_link_packet_t *packet)
{
    if (packet == NULL) return;
    if (s_have_sequence && packet->sequence != s_expected_sequence) {
        ++s_seq_gaps;
        ESP_LOGW(TAG, "UART seq gap expected=%" PRIu32 " got=%" PRIu32,
                 s_expected_sequence, packet->sequence);
    }
    s_expected_sequence = packet->sequence + 1U;
    s_have_sequence = true;

    switch (packet->type) {
    case S3_VOICE_MSG_WAKE:
        dispatch_event(S3_VOICE_EVENT_WAKE, 0U, 0U, 0U, NULL);
        break;
    case S3_VOICE_MSG_TIMEOUT:
        dispatch_event(S3_VOICE_EVENT_TIMEOUT, 0U, 0U, 0U, NULL);
        break;
    case S3_VOICE_MSG_LOCAL_COMMAND:
        if (packet->payload_length == 1U &&
            packet->payload[0] >= VOICE_COMMAND_ID_MIN &&
            packet->payload[0] <= VOICE_COMMAND_ID_MAX)
            dispatch_event(S3_VOICE_EVENT_COMMAND, packet->payload[0],
                           0U, 0U, NULL);
        break;
    case S3_VOICE_MSG_AI_STATE:
        if (packet->payload_length >= 1U)
            dispatch_event(S3_VOICE_EVENT_AI_STATE, 0U,
                           packet->payload[0], 0U, NULL);
        break;
    case S3_VOICE_MSG_AI_AUDIO_START: {
        uint32_t rate = 24000U;
        if (packet->payload_length >= 4U)
            rate = (uint32_t)packet->payload[0] |
                   ((uint32_t)packet->payload[1] << 8U) |
                   ((uint32_t)packet->payload[2] << 16U) |
                   ((uint32_t)packet->payload[3] << 24U);
        reset_pcm_queue();
        s_uart_pcm_received = 0U;
        s_stream_done = false;
        s_stream_finish_queued = false;
        s_stream_active = true;
        dispatch_event(S3_VOICE_EVENT_AI_AUDIO_START, 0U, 0U, rate, NULL);
        break;
    }
    case S3_VOICE_MSG_AI_AUDIO_PCM:
        if (s_stream_active && packet->payload_length > 0U &&
            (packet->payload_length & 1U) == 0U) {
            voice_pcm_frame_t frame = {.length = packet->payload_length};
            memcpy(frame.data, packet->payload, packet->payload_length);
            if (xQueueSend(s_pcm_queue, &frame, 0) != pdTRUE) {
                ++s_queue_overflows;
                if (!s_flow_off) {
                    s_flow_off = true;
                    (void)send_message(S3_VOICE_MSG_FLOW_OFF, NULL, 0U);
                }
            } else {
                s_uart_pcm_received += packet->payload_length;
            }
        }
        break;
    case S3_VOICE_MSG_AI_AUDIO_DONE:
        s_stream_done = true;
        dispatch_event(S3_VOICE_EVENT_AI_AUDIO_DONE, 0U, 0U, 0U, NULL);
        break;
    case S3_VOICE_MSG_AI_TEXT:
        if (packet->payload_length > 0U) {
            memcpy(s_text_buffer, packet->payload, packet->payload_length);
            s_text_buffer[packet->payload_length] = '\0';
            dispatch_event(S3_VOICE_EVENT_AI_TEXT, 0U, 0U, 0U,
                           s_text_buffer);
        }
        break;
    case S3_VOICE_MSG_AI_ERROR:
        dispatch_event(S3_VOICE_EVENT_AI_ERROR, 0U, 0U, 0U, NULL);
        break;
    case S3_VOICE_MSG_STOP_ACK:
        s_stream_active = false;
        s_stream_done = false;
        s_stream_finish_queued = false;
        reset_pcm_queue();
        break;
    default:
        break;
    }
}

static void voice_pcm_task(void *argument)
{
    (void)argument;
    uint64_t next_finish_retry_us = 0U;
    uint64_t next_drain_ack_retry_us = 0U;
    while (true) {
        voice_pcm_frame_t frame;
        if (xQueueReceive(s_pcm_queue, &frame, pdMS_TO_TICKS(20)) == pdTRUE) {
            size_t offset_samples = 0U;
            size_t samples = frame.length / sizeof(int16_t);
            while (offset_samples < samples && s_stream_active) {
                size_t accepted = 0U;
                esp_err_t err = speaker_service_stream_write(
                    ((const int16_t *)frame.data) + offset_samples,
                    samples - offset_samples, &accepted);
                offset_samples += accepted;
                if (offset_samples < samples) {
                    if (err != ESP_OK && err != ESP_ERR_TIMEOUT)
                        ESP_LOGW(TAG, "speaker PCM write failed: %s",
                                 esp_err_to_name(err));
                    vTaskDelay(pdMS_TO_TICKS(2));
                }
            }
        }

        speaker_stream_metrics_t metrics;
        speaker_service_stream_get_metrics(&metrics);
        const uint64_t now = (uint64_t)esp_timer_get_time();
        if (!s_flow_off &&
            metrics.buffered_bytes >= VOICE_FLOW_HIGH_BYTES) {
            s_flow_off = true;
            (void)send_message(S3_VOICE_MSG_FLOW_OFF, NULL, 0U);
        } else if (s_flow_off &&
                   metrics.buffered_bytes <= VOICE_FLOW_LOW_BYTES &&
                   uxQueueSpacesAvailable(s_pcm_queue) >
                       VOICE_PCM_QUEUE_LENGTH / 2U) {
            s_flow_off = false;
            (void)send_message(S3_VOICE_MSG_FLOW_ON, NULL, 0U);
        }

        if (s_stream_active && s_stream_done && !s_stream_finish_queued &&
            uxQueueMessagesWaiting(s_pcm_queue) == 0U &&
            now >= next_finish_retry_us) {
            next_finish_retry_us = now + VOICE_FINISH_RETRY_INTERVAL_US;
            esp_err_t err = speaker_service_stream_finish();
            if (err == ESP_OK) {
                s_stream_done = false;
                s_stream_finish_queued = true;
                next_drain_ack_retry_us = now;
                ESP_LOGI(TAG, "AI PCM producer done; draining speaker ring");
                /* metrics was sampled before FINISH was queued.  Re-enter the
                 * loop so completion is decided from a fresh speaker state. */
                continue;
            } else {
                ESP_LOGW(TAG, "speaker finish request failed: %s; retrying",
                         esp_err_to_name(err));
            }
        }
        /* speaker_service clears its transient finish_requested flag while
         * stopping output, before metrics can expose active=false.  Track the
         * accepted FINISH command locally so the drained state is observable. */
        if (s_stream_active && s_stream_finish_queued &&
            !metrics.active && metrics.buffered_bytes == 0U &&
            now >= next_drain_ack_retry_us) {
            next_drain_ack_retry_us =
                now + VOICE_FINISH_RETRY_INTERVAL_US;
            esp_err_t err = send_message(S3_VOICE_MSG_AI_AUDIO_DRAINED,
                                         NULL, 0U);
            if (err == ESP_OK) {
                s_stream_active = false;
                s_stream_finish_queued = false;
                ESP_LOGI(TAG,
                         "AI PCM drained; P4 completion acknowledged");
            } else {
                ESP_LOGW(TAG,
                         "AI PCM drained ACK failed: %s; retrying",
                         esp_err_to_name(err));
            }
        }

        if (now >= s_next_summary_us) {
            s_next_summary_us = now + VOICE_SUMMARY_INTERVAL_US;
            ESP_LOGI("VOICE_P4",
                     "state=%s uart_pcm_received=%" PRIu64
                     " speaker_buffered=%u played=%" PRIu64
                     " underrun=%u crc_error=%u seq_gap=%u queue=%u",
                     s_stream_active ? "SPEAKING" : "IDLE",
                     s_uart_pcm_received, (unsigned)metrics.buffered_bytes,
                     metrics.played_bytes, (unsigned)metrics.underruns,
                     (unsigned)s_crc_errors, (unsigned)s_seq_gaps,
                     (unsigned)uxQueueMessagesWaiting(s_pcm_queue));
        }
    }
}

static void voice_rx_task(void *argument)
{
    (void)argument;
    uint8_t rx[512];
    bool possible_magic = false;
    bool binary = false;
    s3_voice_link_parser_t parser;
    while (true) {
        int received = uart_read_bytes(
            (uart_port_t)BOARD_WT99_VOICE_UART_PORT, rx, sizeof(rx),
            pdMS_TO_TICKS(100));
        if (received <= 0) continue;
        for (int i = 0; i < received; ++i) {
            const uint8_t byte = rx[i];
            if (binary) {
                s3_voice_link_packet_t packet;
                s3_voice_link_parse_result_t result =
                    s3_voice_link_parser_feed(&parser, byte, &packet);
                if (result == S3_VOICE_LINK_PARSE_COMPLETE) {
                    handle_packet(&packet);
                    binary = false;
                } else if (result == S3_VOICE_LINK_PARSE_ERROR) {
                    ++s_crc_errors;
                    binary = false;
                }
                continue;
            }
            if (possible_magic) {
                possible_magic = false;
                if (byte == S3_VOICE_LINK_MAGIC_1) {
                    s3_voice_link_parser_begin(&parser);
                    binary = true;
                    continue;
                }
            }
            if (byte == S3_VOICE_LINK_MAGIC_0) possible_magic = true;
        }
        size_t buffered = 0U;
        uart_get_buffered_data_len(
            (uart_port_t)BOARD_WT99_VOICE_UART_PORT, &buffered);
        if (buffered >= VOICE_RX_BUFFER_BYTES - 512U) ++s_uart_overflows;
    }
}

esp_err_t s3_voice_node_init(s3_voice_event_handler_t handler, void *context)
{
    if (s_initialized) {
        s_handler = handler;
        s_handler_context = context;
        return ESP_OK;
    }
    if (handler == NULL) return ESP_ERR_INVALID_ARG;
    const uart_config_t config = {
        .baud_rate = BOARD_WT99_VOICE_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(
        (uart_port_t)BOARD_WT99_VOICE_UART_PORT,
        VOICE_RX_BUFFER_BYTES, VOICE_TX_BUFFER_BYTES, 0, NULL, 0);
    if (err == ESP_OK)
        err = uart_param_config((uart_port_t)BOARD_WT99_VOICE_UART_PORT,
                                &config);
    if (err == ESP_OK)
        err = uart_set_pin((uart_port_t)BOARD_WT99_VOICE_UART_PORT,
                           BOARD_WT99_VOICE_UART_TX_GPIO,
                           BOARD_WT99_VOICE_UART_RX_GPIO,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    s_tx_lock = xSemaphoreCreateMutex();
    s_pcm_queue = xQueueCreate(VOICE_PCM_QUEUE_LENGTH,
                               sizeof(voice_pcm_frame_t));
    s_tx_wire = heap_caps_malloc(
        S3_VOICE_LINK_WIRE_OVERHEAD_BYTES +
            S3_VOICE_LINK_MAX_PAYLOAD_BYTES,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_tx_lock == NULL || s_pcm_queue == NULL || s_tx_wire == NULL)
        return ESP_ERR_NO_MEM;
    s_handler = handler;
    s_handler_context = context;
    s_initialized = true;
    if (xTaskCreate(voice_rx_task, "voice_s3_rx", VOICE_RX_TASK_STACK_BYTES,
                    NULL, VOICE_RX_TASK_PRIORITY, NULL) != pdPASS ||
        xTaskCreate(voice_pcm_task, "voice_p4_pcm", VOICE_PCM_TASK_STACK_BYTES,
                    NULL, VOICE_PCM_TASK_PRIORITY, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "UART%d TX=GPIO%d RX=GPIO%d baud=%d protocol=v3 pcm=%u",
             BOARD_WT99_VOICE_UART_PORT,
             BOARD_WT99_VOICE_UART_TX_GPIO,
             BOARD_WT99_VOICE_UART_RX_GPIO,
             BOARD_WT99_VOICE_UART_BAUD_RATE,
             (unsigned)S3_VOICE_LINK_PCM_PAYLOAD_BYTES);
    return ESP_OK;
}

esp_err_t s3_voice_node_send_ack(uint8_t command_id, bool handled)
{
    uint8_t payload[2] = {command_id, handled ? 1U : 0U};
    return send_message(S3_VOICE_MSG_LOCAL_COMMAND_ACK,
                        payload, sizeof(payload));
}

void s3_voice_node_set_audio_handler(s3_voice_audio_handler_t handler,
                                     void *context)
{
    (void)handler;
    (void)context;
    ESP_LOGI(TAG, "legacy P4 microphone uplink handler ignored (runtime disabled)");
}

esp_err_t s3_voice_node_send_ai_stop(const char *reason)
{
    (void)reason;
    s_stream_active = false;
    s_stream_done = false;
    s_stream_finish_queued = false;
    reset_pcm_queue();
    if (speaker_service_is_ready()) (void)speaker_service_stream_abort();
    return send_message(S3_VOICE_MSG_STOP, NULL, 0U);
}

esp_err_t s3_voice_node_send_ai_input_done(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t s3_voice_node_send_wifi_credentials(const char *ssid,
                                               const char *password)
{
    if (ssid == NULL || password == NULL || ssid[0] == '\0')
        return ESP_ERR_INVALID_ARG;
    size_t ssid_length = strlen(ssid);
    size_t password_length = strlen(password);
    if (ssid_length > 32U || password_length > 64U)
        return ESP_ERR_INVALID_SIZE;
    uint8_t payload[2U + 32U + 64U];
    payload[0] = (uint8_t)ssid_length;
    payload[1] = (uint8_t)password_length;
    memcpy(payload + 2U, ssid, ssid_length);
    memcpy(payload + 2U + ssid_length, password, password_length);
    esp_err_t err = send_message(S3_VOICE_MSG_WIFI_CREDENTIALS, payload,
                                 (uint16_t)(2U + ssid_length + password_length));
    memset(payload + 2U + ssid_length, 0, password_length);
    return err;
}
