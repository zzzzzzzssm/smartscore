#include "s3_devices.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_wt99_pins.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CAMERA_RX_BUFFER_BYTES 512
#define CAMERA_LINE_BUFFER_BYTES 64
#define CAMERA_RX_TASK_STACK_BYTES 4096
#define CAMERA_RX_TASK_PRIORITY 5

static const char *TAG = "S3_CAMERA";
static bool s_initialized;
static s3_camera_event_handler_t s_handler;
static void *s_handler_context;
static SemaphoreHandle_t s_tx_lock;

static bool command_supported(uint8_t command_id)
{
    return command_id == 1 || command_id == 2 || command_id == 3 ||
           command_id == 4 || command_id == 6;
}

static void handle_line(char *line)
{
    static const char command_prefix[] = "V1,CMD,";
    if (strncmp(line, command_prefix, sizeof(command_prefix) - 1U) != 0) {
        ESP_LOGW(TAG, "ignored malformed frame: %s", line);
        return;
    }

    char *end = NULL;
    long command_id = strtol(line + sizeof(command_prefix) - 1U, &end, 10);
    if (end == NULL || *end != '\0' || command_id < 0 || command_id > UINT8_MAX ||
        !command_supported((uint8_t)command_id)) {
        ESP_LOGW(TAG, "ignored invalid command frame: %s", line);
        return;
    }

    if (s_handler != NULL) {
        const s3_camera_event_t event = {
            .command_id = (uint8_t)command_id,
        };
        s_handler(&event, s_handler_context);
    }
}

static void camera_rx_task(void *argument)
{
    (void)argument;
    uint8_t rx[64];
    char line[CAMERA_LINE_BUFFER_BYTES];
    size_t line_length = 0;
    bool discarding = false;

    while (true) {
        int received = uart_read_bytes(
            (uart_port_t)BOARD_WT99_CAMERA_UART_PORT, rx, sizeof(rx),
            pdMS_TO_TICKS(100));
        if (received <= 0) continue;

        for (int index = 0; index < received; ++index) {
            char byte = (char)rx[index];
            if (byte == '\r') continue;
            if (byte == '\n') {
                if (!discarding && line_length > 0) {
                    line[line_length] = '\0';
                    handle_line(line);
                }
                line_length = 0;
                discarding = false;
                continue;
            }
            if (discarding) continue;
            if (line_length + 1U < sizeof(line)) {
                line[line_length++] = byte;
            } else {
                ESP_LOGW(TAG, "oversized camera frame discarded");
                discarding = true;
                line_length = 0;
            }
        }
    }
}

esp_err_t s3_camera_node_init(s3_camera_event_handler_t handler,
                              void *context)
{
    if (s_initialized) {
        s_handler = handler;
        s_handler_context = context;
        return ESP_OK;
    }
    if (handler == NULL) return ESP_ERR_INVALID_ARG;

    const uart_config_t config = {
        .baud_rate = BOARD_WT99_CAMERA_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(
        (uart_port_t)BOARD_WT99_CAMERA_UART_PORT,
        CAMERA_RX_BUFFER_BYTES, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(
            (uart_port_t)BOARD_WT99_CAMERA_UART_PORT, &config);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(
            (uart_port_t)BOARD_WT99_CAMERA_UART_PORT,
            BOARD_WT99_CAMERA_UART_TX_GPIO,
            BOARD_WT99_CAMERA_UART_RX_GPIO,
            UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        uart_driver_delete((uart_port_t)BOARD_WT99_CAMERA_UART_PORT);
        return err;
    }

    s_tx_lock = xSemaphoreCreateMutex();
    if (s_tx_lock == NULL) {
        uart_driver_delete((uart_port_t)BOARD_WT99_CAMERA_UART_PORT);
        return ESP_ERR_NO_MEM;
    }
    s_handler = handler;
    s_handler_context = context;
    uart_flush_input((uart_port_t)BOARD_WT99_CAMERA_UART_PORT);
    s_initialized = true;
    if (xTaskCreate(camera_rx_task, "camera_s3_rx",
                    CAMERA_RX_TASK_STACK_BYTES, NULL,
                    CAMERA_RX_TASK_PRIORITY, NULL) != pdPASS) {
        s_initialized = false;
        vSemaphoreDelete(s_tx_lock);
        s_tx_lock = NULL;
        s_handler = NULL;
        s_handler_context = NULL;
        uart_driver_delete((uart_port_t)BOARD_WT99_CAMERA_UART_PORT);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "UART%d TX=GPIO%d RX=GPIO%d at %d baud",
             BOARD_WT99_CAMERA_UART_PORT,
             BOARD_WT99_CAMERA_UART_TX_GPIO,
             BOARD_WT99_CAMERA_UART_RX_GPIO,
             BOARD_WT99_CAMERA_UART_BAUD_RATE);
    return ESP_OK;
}

esp_err_t s3_camera_node_send_ack(uint8_t command_id, bool handled)
{
    if (!s_initialized || s_tx_lock == NULL) return ESP_ERR_INVALID_STATE;
    if (!command_supported(command_id)) return ESP_ERR_INVALID_ARG;

    char line[32];
    int length = snprintf(line, sizeof(line), "V1,ACK,%u,%s\n",
                          (unsigned)command_id,
                          handled ? "OK" : "IGNORED");
    if (length <= 0 || length >= (int)sizeof(line)) return ESP_FAIL;

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int written = uart_write_bytes(
        (uart_port_t)BOARD_WT99_CAMERA_UART_PORT, line, (size_t)length);
    xSemaphoreGive(s_tx_lock);
    return written == length ? ESP_OK : ESP_FAIL;
}

esp_err_t s3_camera_node_send_practice_state(
    s3_camera_practice_state_t state)
{
    if (!s_initialized || s_tx_lock == NULL) return ESP_ERR_INVALID_STATE;

    const char *name = NULL;
    switch (state) {
    case S3_CAMERA_PRACTICE_PLAYING:
        name = "PLAYING";
        break;
    case S3_CAMERA_PRACTICE_PAUSED:
        name = "PAUSED";
        break;
    case S3_CAMERA_PRACTICE_FINISHED:
        name = "FINISHED";
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    char line[32];
    int length = snprintf(line, sizeof(line), "V1,STATE,%s\n", name);
    if (length <= 0 || length >= (int)sizeof(line)) return ESP_FAIL;

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int written = uart_write_bytes(
        (uart_port_t)BOARD_WT99_CAMERA_UART_PORT, line, (size_t)length);
    xSemaphoreGive(s_tx_lock);
    return written == length ? ESP_OK : ESP_FAIL;
}
