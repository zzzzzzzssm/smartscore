#include "p4_control_link.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "board_pins.hpp"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {
constexpr char TAG[] = "p4_control";
constexpr size_t kRxBufferBytes = 512;
constexpr size_t kLineBufferBytes = 64;
constexpr uint32_t kRxTaskStackBytes = 3072;
constexpr UBaseType_t kRxTaskPriority = 5;
constexpr UBaseType_t kAckQueueLength = 8;

bool initialized = false;
QueueHandle_t ack_queue = nullptr;
QueueHandle_t practice_state_queue = nullptr;

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

void handle_practice_state_line(const char *line)
{
    constexpr char prefix[] = "V1,STATE,";
    const char *name = line + sizeof(prefix) - 1U;
    P4PracticeState state;
    if (std::strcmp(name, "PLAYING") == 0) {
        state = P4PracticeState::PLAYING;
    } else if (std::strcmp(name, "PAUSED") == 0) {
        state = P4PracticeState::PAUSED;
    } else if (std::strcmp(name, "FINISHED") == 0) {
        state = P4PracticeState::FINISHED;
    } else {
        ESP_LOGW(TAG, "ignored invalid P4 practice state: %s", line);
        return;
    }

    if (practice_state_queue == nullptr ||
        xQueueOverwrite(practice_state_queue, &state) != pdTRUE) {
        ESP_LOGW(TAG, "unable to queue P4 practice state: %s", name);
        return;
    }
    ESP_LOGI(TAG, "P4 practice state=%s", name);
}

void handle_p4_line(char *line)
{
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
    practice_state_queue = xQueueCreate(1, sizeof(P4PracticeState));
    if (ack_queue == nullptr || practice_state_queue == nullptr) {
        if (ack_queue != nullptr) vQueueDelete(ack_queue);
        if (practice_state_queue != nullptr) {
            vQueueDelete(practice_state_queue);
        }
        ack_queue = nullptr;
        practice_state_queue = nullptr;
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
        ack_queue = nullptr;
        practice_state_queue = nullptr;
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

    const int written = uart_write_bytes(
        static_cast<uart_port_t>(board::P4_LINK_UART_PORT),
        line, static_cast<size_t>(length));
    return written == length ? ESP_OK : ESP_FAIL;
}

bool p4_control_link_receive_ack(P4ControlAck &ack)
{
    return ack_queue != nullptr && xQueueReceive(ack_queue, &ack, 0) == pdTRUE;
}

bool p4_control_link_receive_practice_state(P4PracticeState &state)
{
    return practice_state_queue != nullptr &&
           xQueueReceive(practice_state_queue, &state, 0) == pdTRUE;
}
