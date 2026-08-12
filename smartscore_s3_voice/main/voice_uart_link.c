#include "voice_uart_link.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "voice_ai_stream.h"
#include "voice_config.h"
#include "voice_link_protocol.h"

#define VOICE_LINK_RX_BUFFER_BYTES 512
#define VOICE_LINK_LINE_BYTES 64
#define VOICE_LINK_RX_TASK_STACK_BYTES 3072
#define VOICE_LINK_RX_TASK_PRIORITY 4

static const char *TAG = "voice_uart_link";
static bool s_initialized;
static SemaphoreHandle_t s_tx_lock;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_ai_input_done_requested;
static bool s_ai_stop_requested;

static esp_err_t write_locked(const void *data, size_t length)
{
    if (!s_initialized || s_tx_lock == NULL) return ESP_ERR_INVALID_STATE;
    if (data == NULL || length == 0U) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int written = uart_write_bytes(VOICE_LINK_UART_PORT, data, length);
    xSemaphoreGive(s_tx_lock);
    return written == (int)length ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_frame(const char *frame)
{
    if (frame == NULL) return ESP_ERR_INVALID_ARG;
    return write_locked(frame, strlen(frame));
}

static void handle_ack_line(const char *line)
{
    if (strcmp(line, "V2,AI,INPUT_DONE") == 0) {
        portENTER_CRITICAL(&s_state_lock);
        s_ai_input_done_requested = true;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGI(TAG, "P4 completed the single-turn AI input");
        return;
    }

    if (strcmp(line, "V2,AI,STOP") == 0 ||
        strncmp(line, "V2,AI,STOP,", 11U) == 0) {
        portENTER_CRITICAL(&s_state_lock);
        s_ai_stop_requested = true;
        portEXIT_CRITICAL(&s_state_lock);
        ESP_LOGI(TAG, "P4 requested AI conversation stop: %s", line);
        return;
    }

    unsigned command_id = 0;
    char result[16] = {0};
    if (sscanf(line, "V1,ACK,%u,%15s", &command_id, result) == 2 &&
        command_id >= VOICE_COMMAND_ID_MIN &&
        command_id <= VOICE_COMMAND_ID_MAX &&
        (strcmp(result, "OK") == 0 || strcmp(result, "IGNORED") == 0)) {
        ESP_LOGI(TAG, "P4 command %u: %s", command_id, result);
    } else {
        ESP_LOGW(TAG, "ignored malformed P4 frame: %s", line);
    }
}

static void voice_link_rx_task(void *argument)
{
    (void)argument;
    uint8_t rx[64];
    char line[VOICE_LINK_LINE_BYTES];
    size_t line_length = 0;
    bool discarding = false;
    bool possible_binary = false;
    bool binary = false;
    voice_link_parser_t parser;

    while (true) {
        int received = uart_read_bytes(VOICE_LINK_UART_PORT, rx, sizeof(rx),
                                       pdMS_TO_TICKS(100));
        if (received <= 0) continue;

        for (int index = 0; index < received; ++index) {
            const uint8_t raw = rx[index];
            if (binary) {
                voice_link_packet_t packet;
                voice_link_parse_result_t parse =
                    voice_link_parser_feed(&parser, raw, &packet);
                if (parse == VOICE_LINK_PARSE_COMPLETE) {
                    voice_ai_stream_handle_feedback(packet.type,
                                                    packet.sequence);
                    binary = false;
                } else if (parse == VOICE_LINK_PARSE_ERROR) {
                    ESP_LOGW(TAG, "invalid binary feedback frame discarded");
                    binary = false;
                }
                continue;
            }
            if (possible_binary) {
                possible_binary = false;
                if (raw == VOICE_LINK_MAGIC_1) {
                    voice_link_parser_begin(&parser);
                    binary = true;
                    line_length = 0;
                    discarding = false;
                    continue;
                }
                if (!discarding && line_length + 1U < sizeof(line)) {
                    line[line_length++] = (char)VOICE_LINK_MAGIC_0;
                }
            }
            if (raw == VOICE_LINK_MAGIC_0) {
                possible_binary = true;
                continue;
            }

            char byte = (char)raw;
            if (byte == '\r') continue;
            if (byte == '\n') {
                if (!discarding && line_length > 0) {
                    line[line_length] = '\0';
                    handle_ack_line(line);
                }
                line_length = 0;
                discarding = false;
                continue;
            }
            if (discarding) continue;
            if (line_length + 1U < sizeof(line)) {
                line[line_length++] = byte;
            } else {
                discarding = true;
                line_length = 0;
            }
        }
    }
}

esp_err_t voice_uart_link_init(void)
{
    if (s_initialized) return ESP_OK;

    const uart_config_t config = {
        .baud_rate = VOICE_LINK_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(
        VOICE_LINK_UART_PORT, VOICE_LINK_RX_BUFFER_BYTES, 0, 0, NULL, 0);
    if (err == ESP_OK) err = uart_param_config(VOICE_LINK_UART_PORT, &config);
    if (err == ESP_OK) {
        err = uart_set_pin(VOICE_LINK_UART_PORT,
                           VOICE_LINK_TX_GPIO, VOICE_LINK_RX_GPIO,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        uart_driver_delete(VOICE_LINK_UART_PORT);
        return err;
    }

    s_tx_lock = xSemaphoreCreateMutex();
    if (s_tx_lock == NULL) {
        uart_driver_delete(VOICE_LINK_UART_PORT);
        return ESP_ERR_NO_MEM;
    }
    uart_flush_input(VOICE_LINK_UART_PORT);
    if (xTaskCreate(voice_link_rx_task, "voice_link_rx",
                    VOICE_LINK_RX_TASK_STACK_BYTES, NULL,
                    VOICE_LINK_RX_TASK_PRIORITY, NULL) != pdPASS) {
        vSemaphoreDelete(s_tx_lock);
        s_tx_lock = NULL;
        uart_driver_delete(VOICE_LINK_UART_PORT);
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "UART%d TX=GPIO%d RX=GPIO%d at %d baud",
             VOICE_LINK_UART_PORT, VOICE_LINK_TX_GPIO,
             VOICE_LINK_RX_GPIO, VOICE_LINK_BAUD_RATE);
    return ESP_OK;
}

esp_err_t voice_uart_link_send_wake(void)
{
    return send_frame("V1,WAKE\n");
}

esp_err_t voice_uart_link_send_timeout(void)
{
    return send_frame("V1,TIMEOUT\n");
}

esp_err_t voice_uart_link_send_command(uint8_t command_id)
{
    if (command_id < VOICE_COMMAND_ID_MIN ||
        command_id > VOICE_COMMAND_ID_MAX) return ESP_ERR_INVALID_ARG;

    char frame[24];
    int length = snprintf(frame, sizeof(frame), "V1,CMD,%u\n",
                          (unsigned)command_id);
    if (length <= 0 || length >= (int)sizeof(frame)) return ESP_FAIL;
    return send_frame(frame);
}

esp_err_t voice_uart_link_send_ai_begin(void)
{
    return send_frame("V2,AI,BEGIN\n");
}

esp_err_t voice_uart_link_send_ai_speech_end(void)
{
    return send_frame("V2,AI,SPEECH_END\n");
}

esp_err_t voice_uart_link_send_ai_cancel(void)
{
    return send_frame("V2,AI,CANCEL\n");
}

esp_err_t voice_uart_link_write_binary(const void *data, size_t length)
{
    return write_locked(data, length);
}

bool voice_uart_link_take_ai_stop_request(void)
{
    bool requested;
    portENTER_CRITICAL(&s_state_lock);
    requested = s_ai_stop_requested;
    s_ai_stop_requested = false;
    portEXIT_CRITICAL(&s_state_lock);
    return requested;
}

bool voice_uart_link_take_ai_input_done_request(void)
{
    bool requested;
    portENTER_CRITICAL(&s_state_lock);
    requested = s_ai_input_done_requested;
    s_ai_input_done_requested = false;
    portEXIT_CRITICAL(&s_state_lock);
    return requested;
}
