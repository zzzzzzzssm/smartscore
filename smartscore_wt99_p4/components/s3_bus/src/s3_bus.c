#include "s3_bus.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "board_wt99_pins.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "s3_protocol.h"

#define S3_MUSIC_UART_RX_BUFFER_BYTES 2048
#define S3_MUSIC_UART_TX_BUFFER_BYTES 512
#define S3_MUSIC_LINE_MAX_BYTES       384
#define S3_MUSIC_EVENT_QUEUE_LENGTH   32
#define S3_MUSIC_RX_TASK_STACK_BYTES  6144
#define S3_MUSIC_RX_TASK_PRIORITY     8
#define S3_MUSIC_DISPATCH_STACK_BYTES 4096
#define S3_MUSIC_DISPATCH_PRIORITY    7
#define S3_MUSIC_POLL_STACK_BYTES     3072
#define S3_MUSIC_POLL_PRIORITY        6
#define S3_MUSIC_POLL_INTERVAL_MS     50
#define S3_MUSIC_ONLINE_TIMEOUT_MS    3000
#define S3_MUSIC_COMMAND_MAX_BYTES    64

static const char *TAG = "S3_MUSIC";
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_event_queue;
static SemaphoreHandle_t s_tx_lock;
static TaskHandle_t s_rx_task;
static TaskHandle_t s_dispatch_task;
static TaskHandle_t s_poll_task;
static s3_music_event_handler_t s_handler;
static void *s_handler_context;
static s3_bus_status_t s_status;
static bool s_received_any;
static bool s_sequence_valid;
static bool s_note_stream_logged;
static uint32_t s_requested_sid;

static uint32_t local_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void increment_invalid(void)
{
    taskENTER_CRITICAL(&s_status_lock);
    ++s_status.invalid_frames;
    taskEXIT_CRITICAL(&s_status_lock);
}

static void increment_oversized(void)
{
    taskENTER_CRITICAL(&s_status_lock);
    ++s_status.oversized_lines;
    taskEXIT_CRITICAL(&s_status_lock);
}

static esp_err_t send_command_line(const char *line, size_t length)
{
    if (line == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_status_lock);
    const bool initialized = s_status.initialized;
    taskEXIT_CRITICAL(&s_status_lock);
    if (!initialized || s_tx_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    const int written = uart_write_bytes(
        (uart_port_t)BOARD_WT99_MUSIC_UART_PORT, line, length);
    xSemaphoreGive(s_tx_lock);

    taskENTER_CRITICAL(&s_status_lock);
    ++s_status.tx_commands;
    if (written != (int)length) {
        ++s_status.tx_failures;
    }
    taskEXIT_CRITICAL(&s_status_lock);
    return written == (int)length ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_poll(void)
{
    static const char command[] = "{\"cmd\":\"poll\"}\n";
    const esp_err_t err = send_command_line(command, sizeof(command) - 1U);
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&s_status_lock);
        ++s_status.polls_sent;
        taskEXIT_CRITICAL(&s_status_lock);
    }
    return err;
}

static void music_poll_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(10));
    TickType_t last_wake = xTaskGetTickCount();
    while (true) {
        (void)send_poll();
        taskENTER_CRITICAL(&s_status_lock);
        const uint32_t polls = s_status.polls_sent;
        const uint32_t rx_bytes = s_status.rx_bytes;
        const uint32_t valid = s_status.valid_frames;
        const uint32_t invalid = s_status.invalid_frames;
        const uint32_t oversized = s_status.oversized_lines;
        const uint32_t last_rx_ms = s_status.last_rx_ms;
        const bool received_any = s_received_any;
        taskEXIT_CRITICAL(&s_status_lock);
        if (polls > 0 && polls % 100U == 0U) {
            const uint32_t age_ms = received_any
                                        ? (uint32_t)(local_now_ms() - last_rx_ms)
                                        : UINT32_MAX;
            ESP_LOGI(TAG, "link diag: polls=%" PRIu32 " rx_bytes=%" PRIu32
                          " valid=%" PRIu32 " invalid=%" PRIu32
                          " oversized=%" PRIu32 " last_rx_age_ms=%" PRIu32,
                     polls, rx_bytes, valid, invalid, oversized, age_ms);
            if (rx_bytes == 0) {
                ESP_LOGW(TAG, "no S3 UART bytes: check S3 GPIO1/H7-10 -> P4 GPIO1/J6-6 and GND");
            } else if (valid == 0) {
                ESP_LOGW(TAG, "UART bytes received but no valid NDJSON: check %d 8N1 and TX/RX direction",
                         BOARD_WT99_MUSIC_UART_BAUD_RATE);
            }
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(S3_MUSIC_POLL_INTERVAL_MS));
    }
}

esp_err_t s3_bus_ping(void)
{
    static const char command[] = "{\"cmd\":\"ping\"}\n";
    return send_command_line(command, sizeof(command) - 1U);
}

esp_err_t s3_bus_start_stream(uint32_t sid)
{
    char command[S3_MUSIC_COMMAND_MAX_BYTES];
    const int length = snprintf(command, sizeof(command),
                                "{\"cmd\":\"start\",\"sid\":%" PRIu32 "}\n",
                                sid);
    if (length < 0 || (size_t)length >= sizeof(command)) {
        return ESP_ERR_INVALID_SIZE;
    }
    taskENTER_CRITICAL(&s_status_lock);
    s_status.stream_requested = true;
    s_requested_sid = sid;
    taskEXIT_CRITICAL(&s_status_lock);
    return send_command_line(command, (size_t)length);
}

esp_err_t s3_bus_stop_stream(void)
{
    static const char command[] = "{\"cmd\":\"stop\"}\n";
    taskENTER_CRITICAL(&s_status_lock);
    s_status.stream_requested = false;
    taskEXIT_CRITICAL(&s_status_lock);
    return send_command_line(command, sizeof(command) - 1U);
}

static esp_err_t sync_requested_stream(void)
{
    taskENTER_CRITICAL(&s_status_lock);
    const bool requested = s_status.stream_requested;
    const uint32_t sid = s_requested_sid;
    taskEXIT_CRITICAL(&s_status_lock);
    return requested ? s3_bus_start_stream(sid) : s3_bus_stop_stream();
}

static bool accept_and_record_message(const s3_music_message_t *message)
{
    const uint32_t receive_ms = local_now_ms();
    bool accepted = true;

    taskENTER_CRITICAL(&s_status_lock);
    const bool first_valid_frame = !s_received_any;
    s_received_any = true;
    s_status.last_rx_ms = receive_ms;
    s_status.last_sender_ts_ms = message->ts_ms;

    const bool new_session =
        message->type == S3_MUSIC_MESSAGE_HELLO ||
        !s_sequence_valid ||
        message->sid != s_status.last_sid;
    if (!new_session &&
        (int32_t)(message->seq - s_status.last_seq) <= 0) {
        ++s_status.duplicate_frames;
        accepted = false;
    } else {
        s_sequence_valid = true;
        s_status.last_sid = message->sid;
        s_status.last_seq = message->seq;
        ++s_status.valid_frames;

        if (message->type == S3_MUSIC_MESSAGE_STATUS) {
            s_status.ready = message->ready;
            s_status.stream_enabled = message->stream_enabled;
        } else if (message->type == S3_MUSIC_MESSAGE_HEARTBEAT) {
            s_status.ready = message->ready;
        } else if (message->type == S3_MUSIC_MESSAGE_PONG) {
            s_status.command_link_confirmed = true;
            s_status.last_pong_ms = receive_ms;
        }
    }
    taskEXIT_CRITICAL(&s_status_lock);
    if (first_valid_frame) {
        ESP_LOGI(TAG, "RX path confirmed: first valid S3 frame received on GPIO%d",
                 BOARD_WT99_MUSIC_UART_RX_GPIO);
    }
    return accepted;
}

static void queue_note_event(const s3_music_message_t *message)
{
    s3_music_event_t event = {
        .type = message->type == S3_MUSIC_MESSAGE_NOTE_ON
                    ? S3_MUSIC_EVENT_NOTE_ON
                    : S3_MUSIC_EVENT_NOTE_OFF,
        .seq = message->seq,
        .sid = message->sid,
        .sender_ts_ms = message->ts_ms,
        .midi = message->midi,
        .velocity = message->velocity,
    };
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        taskENTER_CRITICAL(&s_status_lock);
        ++s_status.dropped_events;
        taskEXIT_CRITICAL(&s_status_lock);
    }
}

static void process_line(char *line, size_t length)
{
    line[length] = '\0';
    s3_music_message_t message;
    const s3_protocol_result_t result =
        s3_protocol_parse_music_line(line, length, &message);
    if (result != S3_PROTOCOL_OK) {
        increment_invalid();
        return;
    }
    if (!accept_and_record_message(&message)) {
        return;
    }
    if (message.type == S3_MUSIC_MESSAGE_HELLO) {
        esp_err_t err = sync_requested_stream();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to synchronize S3 stream state: %s",
                     esp_err_to_name(err));
        }
        err = s3_bus_ping();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to ping S3: %s", esp_err_to_name(err));
        }
    } else if (message.type == S3_MUSIC_MESSAGE_PONG) {
        ESP_LOGI(TAG, "bidirectional UART confirmed by S3 pong (sid=%" PRIu32
                      ", seq=%" PRIu32 ")",
                 message.sid, message.seq);
    }
    if (message.type == S3_MUSIC_MESSAGE_NOTE_ON ||
        message.type == S3_MUSIC_MESSAGE_NOTE_OFF) {
        if (!s_note_stream_logged) {
            ESP_LOGI(TAG,
                     "S3 melody note stream confirmed: type=%s midi=%u velocity=%u",
                     message.type == S3_MUSIC_MESSAGE_NOTE_ON ? "note_on"
                                                              : "note_off",
                     (unsigned)message.midi, (unsigned)message.velocity);
            s_note_stream_logged = true;
        }
        queue_note_event(&message);
    }
}

static void music_rx_task(void *argument)
{
    (void)argument;
    uint8_t input[64];
    char line[S3_MUSIC_LINE_MAX_BYTES + 1];
    size_t line_length = 0;
    bool overflow = false;

    while (true) {
        const int received = uart_read_bytes(
            (uart_port_t)BOARD_WT99_MUSIC_UART_PORT,
            input, sizeof(input), pdMS_TO_TICKS(50));
        if (received <= 0) {
            continue;
        }
        taskENTER_CRITICAL(&s_status_lock);
        const bool first_uart_bytes = s_status.rx_bytes == 0;
        s_status.rx_bytes += (uint32_t)received;
        taskEXIT_CRITICAL(&s_status_lock);
        if (first_uart_bytes) {
            ESP_LOGI(TAG, "electrical RX activity detected on GPIO%d (%d bytes)",
                     BOARD_WT99_MUSIC_UART_RX_GPIO, received);
        }
        for (int i = 0; i < received; ++i) {
            const char character = (char)input[i];
            if (character == '\n') {
                if (overflow) {
                    increment_oversized();
                } else if (line_length > 0) {
                    process_line(line, line_length);
                }
                line_length = 0;
                overflow = false;
            } else if (character != '\r' && !overflow) {
                if (line_length < S3_MUSIC_LINE_MAX_BYTES) {
                    line[line_length++] = character;
                } else {
                    overflow = true;
                }
            }
        }
    }
}

static void music_dispatch_task(void *argument)
{
    (void)argument;
    s3_music_event_t event;
    while (true) {
        if (xQueueReceive(s_event_queue, &event, portMAX_DELAY) == pdTRUE &&
            s_handler != NULL) {
            s_handler(&event, s_handler_context);
        }
    }
}

static void cleanup_failed_init(void)
{
    if (s_poll_task != NULL) {
        vTaskDelete(s_poll_task);
        s_poll_task = NULL;
    }
    if (s_rx_task != NULL) {
        vTaskDelete(s_rx_task);
        s_rx_task = NULL;
    }
    if (s_dispatch_task != NULL) {
        vTaskDelete(s_dispatch_task);
        s_dispatch_task = NULL;
    }
    if (s_event_queue != NULL) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
    }
    if (s_tx_lock != NULL) {
        vSemaphoreDelete(s_tx_lock);
        s_tx_lock = NULL;
    }
    uart_driver_delete((uart_port_t)BOARD_WT99_MUSIC_UART_PORT);
    s_handler = NULL;
    s_handler_context = NULL;
}

esp_err_t s3_bus_init(s3_music_event_handler_t handler, void *context)
{
    taskENTER_CRITICAL(&s_status_lock);
    const bool initialized = s_status.initialized;
    taskEXIT_CRITICAL(&s_status_lock);
    if (initialized) {
        return ESP_OK;
    }
    if (handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_handler = handler;
    s_handler_context = context;
    s_tx_lock = xSemaphoreCreateMutex();
    s_event_queue = xQueueCreate(S3_MUSIC_EVENT_QUEUE_LENGTH,
                                 sizeof(s3_music_event_t));
    if (s_tx_lock == NULL || s_event_queue == NULL) {
        cleanup_failed_init();
        return ESP_ERR_NO_MEM;
    }

    const uart_config_t uart_config = {
        .baud_rate = BOARD_WT99_MUSIC_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {0},
    };
    esp_err_t err = uart_driver_install(
        (uart_port_t)BOARD_WT99_MUSIC_UART_PORT,
        S3_MUSIC_UART_RX_BUFFER_BYTES,
        S3_MUSIC_UART_TX_BUFFER_BYTES, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(
            (uart_port_t)BOARD_WT99_MUSIC_UART_PORT, &uart_config);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(
            (uart_port_t)BOARD_WT99_MUSIC_UART_PORT,
            BOARD_WT99_MUSIC_UART_TX_GPIO,
            BOARD_WT99_MUSIC_UART_RX_GPIO,
            UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        cleanup_failed_init();
        return err;
    }
    uart_flush_input((uart_port_t)BOARD_WT99_MUSIC_UART_PORT);

    if (xTaskCreate(music_dispatch_task, "s3_music_dispatch",
                    S3_MUSIC_DISPATCH_STACK_BYTES, NULL,
                    S3_MUSIC_DISPATCH_PRIORITY,
                    &s_dispatch_task) != pdPASS ||
        xTaskCreate(music_rx_task, "s3_music_rx",
                    S3_MUSIC_RX_TASK_STACK_BYTES, NULL,
                    S3_MUSIC_RX_TASK_PRIORITY,
                    &s_rx_task) != pdPASS ||
        xTaskCreate(music_poll_task, "s3_music_poll",
                    S3_MUSIC_POLL_STACK_BYTES, NULL,
                    S3_MUSIC_POLL_PRIORITY,
                    &s_poll_task) != pdPASS) {
        cleanup_failed_init();
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_status_lock);
    s_status.initialized = true;
    taskEXIT_CRITICAL(&s_status_lock);
    ESP_LOGI(TAG, "master-poll UART%d TX=GPIO%d RX=GPIO%d at %d baud (%d ms)",
             BOARD_WT99_MUSIC_UART_PORT,
             BOARD_WT99_MUSIC_UART_TX_GPIO,
             BOARD_WT99_MUSIC_UART_RX_GPIO,
             BOARD_WT99_MUSIC_UART_BAUD_RATE,
             S3_MUSIC_POLL_INTERVAL_MS);

    /* The S3 buffers all uplink frames until this P4 polls it. Keep recognition
     * quiet until audio_s3 is selected, then verify the command path by pong. */
    (void)sync_requested_stream();
    (void)s3_bus_ping();
    return ESP_OK;
}

void s3_bus_get_status(s3_bus_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_status_lock);
    *out_status = s_status;
    const bool received_any = s_received_any;
    taskEXIT_CRITICAL(&s_status_lock);

    out_status->online =
        out_status->initialized && received_any &&
        (uint32_t)(local_now_ms() - out_status->last_rx_ms) <=
            S3_MUSIC_ONLINE_TIMEOUT_MS;
}
