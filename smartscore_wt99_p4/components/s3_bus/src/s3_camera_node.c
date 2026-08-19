#include "s3_devices.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_wt99_pins.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CAMERA_RX_BUFFER_BYTES 4096
#define CAMERA_LINE_BUFFER_BYTES 160
#define CAMERA_RX_TASK_STACK_BYTES 4096
#define CAMERA_RX_TASK_PRIORITY 5
#define CAMERA_PHOTO_CHUNK_BYTES 1024
#define CAMERA_PHOTO_RESPONSE_TIMEOUT_MS 5000
#define CAMERA_PHOTO_CHUNK_TIMEOUT_MS 5000

static const char *TAG = "S3_CAMERA";
static bool s_initialized;
static s3_camera_event_handler_t s_handler;
static void *s_handler_context;
static SemaphoreHandle_t s_tx_lock;
static SemaphoreHandle_t s_photo_request_lock;
static QueueHandle_t s_photo_response_queue;
static QueueHandle_t s_photo_chunk_queue;
static size_t s_binary_remaining;

typedef enum {
    CAMERA_PHOTO_RESPONSE_LIST = 1,
    CAMERA_PHOTO_RESPONSE_DATA,
    CAMERA_PHOTO_RESPONSE_ERROR,
} camera_photo_response_type_t;

typedef struct {
    camera_photo_response_type_t type;
    size_t value;
} camera_photo_response_t;

typedef struct {
    size_t length;
    uint8_t data[CAMERA_PHOTO_CHUNK_BYTES];
} camera_photo_chunk_t;

static camera_photo_chunk_t s_rx_photo_chunk;

static bool command_supported(uint8_t command_id)
{
    return command_id == 1 || command_id == 2 || command_id == 3 ||
           command_id == 4 || command_id == 6;
}

static bool valid_session_id(const char *value)
{
    if (value == NULL || value[0] == '\0' || strlen(value) >= 64) return false;
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != '\0'; ++cursor) {
        bool allowed = (*cursor >= 'a' && *cursor <= 'z') ||
                       (*cursor >= 'A' && *cursor <= 'Z') ||
                       (*cursor >= '0' && *cursor <= '9') ||
                       *cursor == '-' || *cursor == '_';
        if (!allowed) return false;
    }
    return true;
}

static bool parse_positive_size(const char *value, size_t *out_value)
{
    if (value == NULL || out_value == NULL || value[0] == '\0') return false;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end == NULL || *end != '\0' || parsed == 0) return false;
    *out_value = (size_t)parsed;
    return true;
}

static bool handle_photo_line(char *line)
{
    camera_photo_response_t response = {0};
    static const char list_prefix[] = "V1,PHOTO,LIST,";
    static const char data_prefix[] = "V1,PHOTO,DATA,";
    static const char error_prefix[] = "V1,PHOTO,ERROR,";
    if (strncmp(line, list_prefix, sizeof(list_prefix) - 1U) == 0) {
        response.type = CAMERA_PHOTO_RESPONSE_LIST;
        if (!parse_positive_size(line + sizeof(list_prefix) - 1U,
                                 &response.value)) {
            response.value = 0;
        }
    } else if (strncmp(line, data_prefix, sizeof(data_prefix) - 1U) == 0) {
        response.type = CAMERA_PHOTO_RESPONSE_DATA;
        if (!parse_positive_size(line + sizeof(data_prefix) - 1U,
                                 &response.value)) {
            response.type = CAMERA_PHOTO_RESPONSE_ERROR;
        } else {
            s_binary_remaining = response.value;
            s_rx_photo_chunk.length = 0;
        }
    } else if (strncmp(line, error_prefix, sizeof(error_prefix) - 1U) == 0) {
        response.type = CAMERA_PHOTO_RESPONSE_ERROR;
    } else {
        return false;
    }
    if (s_photo_response_queue != NULL) {
        (void)xQueueSend(s_photo_response_queue, &response, 0);
    }
    return true;
}

static void handle_line(char *line)
{
    if (handle_photo_line(line)) return;
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
    uint8_t rx[256];
    char line[CAMERA_LINE_BUFFER_BYTES];
    size_t line_length = 0;
    bool discarding = false;

    while (true) {
        int received = uart_read_bytes(
            (uart_port_t)BOARD_WT99_CAMERA_UART_PORT, rx, sizeof(rx),
            pdMS_TO_TICKS(100));
        if (received <= 0) continue;

        for (int index = 0; index < received;) {
            if (s_binary_remaining > 0) {
                size_t available = (size_t)(received - index);
                size_t capacity = sizeof(s_rx_photo_chunk.data) -
                                  s_rx_photo_chunk.length;
                size_t take = available < capacity ? available : capacity;
                if (take > s_binary_remaining) take = s_binary_remaining;
                memcpy(s_rx_photo_chunk.data + s_rx_photo_chunk.length,
                       rx + index, take);
                s_rx_photo_chunk.length += take;
                s_binary_remaining -= take;
                index += (int)take;
                if (s_rx_photo_chunk.length == sizeof(s_rx_photo_chunk.data) ||
                    s_binary_remaining == 0) {
                    if (s_photo_chunk_queue != NULL) {
                        (void)xQueueSend(s_photo_chunk_queue,
                                         &s_rx_photo_chunk,
                                         portMAX_DELAY);
                    }
                    s_rx_photo_chunk.length = 0;
                }
                continue;
            }
            char byte = (char)rx[index];
            ++index;
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
    s_photo_request_lock = xSemaphoreCreateMutex();
    s_photo_response_queue = xQueueCreate(2, sizeof(camera_photo_response_t));
    s_photo_chunk_queue = xQueueCreate(4, sizeof(camera_photo_chunk_t));
    if (s_tx_lock == NULL || s_photo_request_lock == NULL ||
        s_photo_response_queue == NULL || s_photo_chunk_queue == NULL) {
        if (s_tx_lock != NULL) vSemaphoreDelete(s_tx_lock);
        if (s_photo_request_lock != NULL) vSemaphoreDelete(s_photo_request_lock);
        if (s_photo_response_queue != NULL) vQueueDelete(s_photo_response_queue);
        if (s_photo_chunk_queue != NULL) vQueueDelete(s_photo_chunk_queue);
        s_tx_lock = NULL;
        s_photo_request_lock = NULL;
        s_photo_response_queue = NULL;
        s_photo_chunk_queue = NULL;
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
        vSemaphoreDelete(s_photo_request_lock);
        vQueueDelete(s_photo_response_queue);
        vQueueDelete(s_photo_chunk_queue);
        s_tx_lock = NULL;
        s_photo_request_lock = NULL;
        s_photo_response_queue = NULL;
        s_photo_chunk_queue = NULL;
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
    s3_camera_practice_state_t state,
    const char *practice_session_id)
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

    const char *session = valid_session_id(practice_session_id)
                              ? practice_session_id : "";
    char line[128];
    int length = snprintf(line, sizeof(line), "V1,STATE,%s,%s\n",
                          name, session);
    if (length <= 0 || length >= (int)sizeof(line)) return ESP_FAIL;

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int written = uart_write_bytes(
        (uart_port_t)BOARD_WT99_CAMERA_UART_PORT, line, (size_t)length);
    xSemaphoreGive(s_tx_lock);
    return written == length ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_photo_request_and_wait(const char *line,
                                             camera_photo_response_t *response)
{
    if (!s_initialized || s_tx_lock == NULL ||
        s_photo_request_lock == NULL || response == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_photo_request_lock, portMAX_DELAY);
    xQueueReset(s_photo_response_queue);
    xQueueReset(s_photo_chunk_queue);
    s_binary_remaining = 0;
    s_rx_photo_chunk.length = 0;

    size_t length = strlen(line);
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int written = uart_write_bytes(
        (uart_port_t)BOARD_WT99_CAMERA_UART_PORT, line, length);
    xSemaphoreGive(s_tx_lock);
    if (written != (int)length) {
        return ESP_FAIL;
    }
    if (xQueueReceive(s_photo_response_queue, response,
                      pdMS_TO_TICKS(CAMERA_PHOTO_RESPONSE_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t s3_camera_node_list_photos(const char *practice_session_id,
                                     uint32_t *out_photo_count)
{
    if (!valid_session_id(practice_session_id) || out_photo_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_photo_request_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    char line[128];
    int length = snprintf(line, sizeof(line), "V1,PHOTO,LIST,%s\n",
                          practice_session_id);
    if (length <= 0 || length >= (int)sizeof(line)) return ESP_ERR_INVALID_SIZE;
    camera_photo_response_t response = {0};
    esp_err_t err = send_photo_request_and_wait(line, &response);
    if (err == ESP_OK) {
        if (response.type != CAMERA_PHOTO_RESPONSE_LIST) {
            err = ESP_ERR_NOT_FOUND;
        } else {
            *out_photo_count = (uint32_t)response.value;
        }
    }
    xSemaphoreGive(s_photo_request_lock);
    return err;
}

esp_err_t s3_camera_node_stream_photo(
    const char *practice_session_id,
    uint32_t photo_index,
    s3_camera_photo_chunk_handler_t handler,
    void *context)
{
    if (!valid_session_id(practice_session_id) || photo_index == 0 ||
        photo_index > 999999 || handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized || s_photo_request_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    char line[144];
    int length = snprintf(line, sizeof(line), "V1,PHOTO,GET,%s,%lu\n",
                          practice_session_id, (unsigned long)photo_index);
    if (length <= 0 || length >= (int)sizeof(line)) return ESP_ERR_INVALID_SIZE;
    camera_photo_response_t response = {0};
    esp_err_t err = send_photo_request_and_wait(line, &response);
    if (err != ESP_OK || response.type != CAMERA_PHOTO_RESPONSE_DATA ||
        response.value == 0) {
        xSemaphoreGive(s_photo_request_lock);
        return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
    }

    const size_t total = response.value;
    size_t delivered = 0;
    bool first = true;
    esp_err_t callback_error = ESP_OK;
    while (delivered < total) {
        camera_photo_chunk_t chunk = {0};
        if (xQueueReceive(s_photo_chunk_queue, &chunk,
                          pdMS_TO_TICKS(CAMERA_PHOTO_CHUNK_TIMEOUT_MS)) != pdTRUE ||
            chunk.length == 0 || chunk.length > total - delivered) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (callback_error == ESP_OK) {
            callback_error = handler(chunk.data, chunk.length, total,
                                     first, context);
        }
        first = false;
        delivered += chunk.length;
    }
    xSemaphoreGive(s_photo_request_lock);
    if (err != ESP_OK) return err;
    return callback_error;
}
