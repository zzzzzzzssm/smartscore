#include "p4_control_link.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "board_pins.hpp"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_storage.hpp"

namespace {
constexpr char TAG[] = "p4_control";
constexpr size_t kRxBufferBytes = 512;
constexpr size_t kLineBufferBytes = 160;
constexpr uint32_t kRxTaskStackBytes = 6144;
constexpr UBaseType_t kRxTaskPriority = 5;
constexpr UBaseType_t kAckQueueLength = 8;

bool initialized = false;
QueueHandle_t ack_queue = nullptr;
QueueHandle_t practice_state_queue = nullptr;
SemaphoreHandle_t tx_lock = nullptr;
SdStorage *photo_storage = nullptr;

bool write_uart(const void *data, size_t length)
{
    if (!initialized || data == nullptr || length == 0 || tx_lock == nullptr) {
        return false;
    }
    xSemaphoreTake(tx_lock, portMAX_DELAY);
    const int written = uart_write_bytes(
        static_cast<uart_port_t>(board::P4_LINK_UART_PORT),
        data, length);
    xSemaphoreGive(tx_lock);
    return written == static_cast<int>(length);
}

void send_photo_error(const char *code)
{
    char line[64] = {};
    const int length = std::snprintf(line, sizeof(line),
                                     "V1,PHOTO,ERROR,%s\n",
                                     code == nullptr ? "FAILED" : code);
    if (length > 0 && length < static_cast<int>(sizeof(line))) {
        (void)write_uart(line, static_cast<size_t>(length));
    }
}

void handle_photo_request(char *line)
{
    constexpr char list_prefix[] = "V1,PHOTO,LIST,";
    constexpr char get_prefix[] = "V1,PHOTO,GET,";
    if (photo_storage == nullptr) {
        send_photo_error("NOT_READY");
        return;
    }
    if (std::strncmp(line, list_prefix, sizeof(list_prefix) - 1U) == 0) {
        const char *session_id = line + sizeof(list_prefix) - 1U;
        uint32_t count = 0;
        char directory[vision_config::kSessionPathBufferSize] = {};
        if (!photo_storage->find_photo_session(session_id, &count,
                                               directory,
                                               sizeof(directory))) {
            send_photo_error("NOT_FOUND");
            return;
        }
        char response[64] = {};
        const int length = std::snprintf(response, sizeof(response),
                                         "V1,PHOTO,LIST,%" PRIu32 "\n",
                                         count);
        if (length <= 0 || length >= static_cast<int>(sizeof(response)) ||
            !write_uart(response, static_cast<size_t>(length))) {
            ESP_LOGW(TAG, "unable to send photo list response");
        }
        return;
    }
    if (std::strncmp(line, get_prefix, sizeof(get_prefix) - 1U) != 0) {
        send_photo_error("BAD_REQUEST");
        return;
    }

    char *session_id = line + sizeof(get_prefix) - 1U;
    char *separator = std::strrchr(session_id, ',');
    if (separator == nullptr) {
        send_photo_error("BAD_REQUEST");
        return;
    }
    *separator = '\0';
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(separator + 1, &end, 10);
    if (separator[1] == '\0' || end == nullptr || *end != '\0' ||
        parsed == 0 || parsed > 999999UL) {
        send_photo_error("BAD_REQUEST");
        return;
    }

    char path[vision_config::kPhotoPathBufferSize] = {};
    size_t file_size = 0;
    if (!photo_storage->photo_file_info(session_id,
                                        static_cast<uint32_t>(parsed),
                                        path, sizeof(path), &file_size)) {
        send_photo_error("NOT_FOUND");
        return;
    }
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        send_photo_error("READ_FAILED");
        return;
    }

    char header[64] = {};
    const int header_length = std::snprintf(header, sizeof(header),
                                            "V1,PHOTO,DATA,%u\n",
                                            static_cast<unsigned>(file_size));
    if (header_length <= 0 ||
        header_length >= static_cast<int>(sizeof(header))) {
        std::fclose(file);
        send_photo_error("READ_FAILED");
        return;
    }

    xSemaphoreTake(tx_lock, portMAX_DELAY);
    bool success = uart_write_bytes(
                       static_cast<uart_port_t>(board::P4_LINK_UART_PORT),
                       header, static_cast<size_t>(header_length)) ==
                   header_length;
    uint8_t buffer[1024];
    while (success) {
        const size_t count = std::fread(buffer, 1, sizeof(buffer), file);
        if (count == 0) break;
        success = uart_write_bytes(
                      static_cast<uart_port_t>(board::P4_LINK_UART_PORT),
                      buffer, count) == static_cast<int>(count);
    }
    xSemaphoreGive(tx_lock);
    std::fclose(file);
    if (!success) ESP_LOGW(TAG, "photo UART stream failed: %s", path);
}

bool command_supported(long command_id)
{
    return command_id == static_cast<long>(P4ControlCommand::START_PRACTICE) ||
           command_id == static_cast<long>(P4ControlCommand::PAUSE_PRACTICE) ||
           command_id == static_cast<long>(P4ControlCommand::NEXT_PAGE) ||
           command_id == static_cast<long>(P4ControlCommand::PREVIOUS_PAGE) ||
           command_id == static_cast<long>(P4ControlCommand::SHOW_SCORE);
}

void handle_ack_line(char *line)
{
    constexpr char prefix[] = "V1,ACK,";
    if (std::strncmp(line, prefix, sizeof(prefix) - 1U) != 0) {
        ESP_LOGW(TAG, "ignored malformed P4 frame: %s", line);
        return;
    }

    char *end = nullptr;
    const long command_id = std::strtol(line + sizeof(prefix) - 1U, &end, 10);
    if (end == nullptr || *end != ',' || !command_supported(command_id)) {
        ESP_LOGW(TAG, "ignored invalid P4 ACK: %s", line);
        return;
    }

    const char *result = end + 1;
    P4ControlAck ack;
    ack.command = static_cast<P4ControlCommand>(command_id);
    if (std::strcmp(result, "OK") == 0) {
        ack.handled = true;
        ESP_LOGI(TAG, "P4 handled command %ld", command_id);
    } else if (std::strcmp(result, "IGNORED") == 0) {
        ack.handled = false;
        ESP_LOGI(TAG, "P4 ignored command %ld on the current page", command_id);
    } else {
        ESP_LOGW(TAG, "ignored invalid P4 ACK result: %s", line);
        return;
    }
    if (ack_queue == nullptr || xQueueSend(ack_queue, &ack, 0) != pdTRUE) {
        ESP_LOGW(TAG, "P4 ACK queue full; command %ld result dropped", command_id);
    }
}

void handle_practice_state_line(char *line)
{
    constexpr char prefix[] = "V1,STATE,";
    char *name = line + sizeof(prefix) - 1U;
    char *session_id = std::strchr(name, ',');
    if (session_id != nullptr) {
        *session_id++ = '\0';
    }
    P4PracticeStateEvent event;
    if (std::strcmp(name, "PLAYING") == 0) {
        event.state = P4PracticeState::PLAYING;
    } else if (std::strcmp(name, "PAUSED") == 0) {
        event.state = P4PracticeState::PAUSED;
    } else if (std::strcmp(name, "FINISHED") == 0) {
        event.state = P4PracticeState::FINISHED;
    } else {
        ESP_LOGW(TAG, "ignored invalid P4 practice state: %s", line);
        return;
    }
    if (session_id != nullptr) {
        std::snprintf(event.session_id, sizeof(event.session_id), "%s",
                      session_id);
    }

    if (practice_state_queue == nullptr ||
        xQueueOverwrite(practice_state_queue, &event) != pdTRUE) {
        ESP_LOGW(TAG, "unable to queue P4 practice state: %s", name);
        return;
    }
    ESP_LOGI(TAG, "P4 practice state=%s", name);
}

void handle_p4_line(char *line)
{
    if (std::strncmp(line, "V1,PHOTO,", 9) == 0) {
        handle_photo_request(line);
        return;
    }
    constexpr char state_prefix[] = "V1,STATE,";
    if (std::strncmp(line, state_prefix, sizeof(state_prefix) - 1U) == 0) {
        handle_practice_state_line(line);
        return;
    }
    handle_ack_line(line);
}

void p4_rx_task(void *argument)
{
    (void)argument;
    uint8_t rx[64];
    char line[kLineBufferBytes];
    size_t line_length = 0;
    bool discarding = false;

    while (true) {
        const int received = uart_read_bytes(
            static_cast<uart_port_t>(board::P4_LINK_UART_PORT),
            rx, sizeof(rx), pdMS_TO_TICKS(100));
        if (received <= 0) continue;

        for (int index = 0; index < received; ++index) {
            const char byte = static_cast<char>(rx[index]);
            if (byte == '\r') continue;
            if (byte == '\n') {
                if (!discarding && line_length > 0) {
                    line[line_length] = '\0';
                    handle_p4_line(line);
                }
                line_length = 0;
                discarding = false;
                continue;
            }
            if (discarding) continue;
            if (line_length + 1U < sizeof(line)) {
                line[line_length++] = byte;
            } else {
                ESP_LOGW(TAG, "oversized P4 frame discarded");
                discarding = true;
                line_length = 0;
            }
        }
    }
}
} // namespace

esp_err_t p4_control_link_init()
{
    if (initialized) return ESP_OK;

    uart_config_t config{};
    config.baud_rate = board::P4_LINK_UART_BAUD_RATE;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;

    const uart_port_t port =
        static_cast<uart_port_t>(board::P4_LINK_UART_PORT);
    esp_err_t err = uart_driver_install(port, kRxBufferBytes, 0, 0, nullptr, 0);
    if (err == ESP_OK) err = uart_param_config(port, &config);
    if (err == ESP_OK) {
        err = uart_set_pin(port,
                           board::P4_LINK_UART_TX,
                           board::P4_LINK_UART_RX,
                           UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        uart_driver_delete(port);
        return err;
    }

    ack_queue = xQueueCreate(kAckQueueLength, sizeof(P4ControlAck));
    practice_state_queue = xQueueCreate(1, sizeof(P4PracticeStateEvent));
    tx_lock = xSemaphoreCreateMutex();
    if (ack_queue == nullptr || practice_state_queue == nullptr ||
        tx_lock == nullptr) {
        if (ack_queue != nullptr) vQueueDelete(ack_queue);
        if (practice_state_queue != nullptr) {
            vQueueDelete(practice_state_queue);
        }
        if (tx_lock != nullptr) vSemaphoreDelete(tx_lock);
        ack_queue = nullptr;
        practice_state_queue = nullptr;
        tx_lock = nullptr;
        uart_driver_delete(port);
        return ESP_ERR_NO_MEM;
    }
    uart_flush_input(port);
    initialized = true;
    if (xTaskCreate(p4_rx_task, "p4_control_rx", kRxTaskStackBytes,
                    nullptr, kRxTaskPriority, nullptr) != pdPASS) {
        initialized = false;
        vQueueDelete(ack_queue);
        vQueueDelete(practice_state_queue);
        vSemaphoreDelete(tx_lock);
        ack_queue = nullptr;
        practice_state_queue = nullptr;
        tx_lock = nullptr;
        uart_driver_delete(port);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "UART%d TX=GPIO%d RX=GPIO%d at %d baud",
             board::P4_LINK_UART_PORT,
             static_cast<int>(board::P4_LINK_UART_TX),
             static_cast<int>(board::P4_LINK_UART_RX),
             board::P4_LINK_UART_BAUD_RATE);
    return ESP_OK;
}

esp_err_t p4_control_link_send(P4ControlCommand command)
{
    const long command_id = static_cast<long>(command);
    if (!command_supported(command_id)) return ESP_ERR_INVALID_ARG;
    if (!initialized) return ESP_ERR_INVALID_STATE;

    char line[24];
    const int length = std::snprintf(line, sizeof(line), "V1,CMD,%ld\n",
                                     command_id);
    if (length <= 0 || length >= static_cast<int>(sizeof(line))) {
        return ESP_FAIL;
    }

    return write_uart(line, static_cast<size_t>(length)) ? ESP_OK : ESP_FAIL;
}

void p4_control_link_set_photo_storage(SdStorage *storage)
{
    photo_storage = storage;
}

bool p4_control_link_receive_ack(P4ControlAck &ack)
{
    return ack_queue != nullptr && xQueueReceive(ack_queue, &ack, 0) == pdTRUE;
}

bool p4_control_link_receive_practice_state(P4PracticeStateEvent &event)
{
    return practice_state_queue != nullptr &&
           xQueueReceive(practice_state_queue, &event, 0) == pdTRUE;
}
