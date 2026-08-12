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
#include "s3_voice_adpcm.h"
#include "s3_voice_link_protocol.h"

#define VOICE_RX_BUFFER_BYTES 2048
#define VOICE_LINE_BUFFER_BYTES 64
#define VOICE_RX_TASK_STACK_BYTES 4096
#define VOICE_RX_TASK_PRIORITY 5
#define VOICE_COMMAND_ID_MIN 1
#define VOICE_COMMAND_ID_MAX 7
#define VOICE_PCM_FRAME_SAMPLES 320
#define VOICE_IDLE_RESYNC_INTERVAL_MS 1000

static const char *TAG = "S3_VOICE";
static bool s_initialized;
static s3_voice_event_handler_t s_handler;
static void *s_handler_context;
static SemaphoreHandle_t s_tx_lock;
static s3_voice_audio_handler_t s_audio_handler;
static void *s_audio_context;
static uint16_t s_expected_audio_sequence;
static bool s_expect_audio;
static bool s_drop_audio_until_stop;
static uint32_t s_idle_discarded_frames;
static TickType_t s_idle_last_resync_tick;

static void reset_idle_audio_resync(void)
{
    s_idle_discarded_frames = 0;
    s_idle_last_resync_tick = 0;
}

static esp_err_t write_locked(const void *data, size_t length)
{
    if (!s_initialized || s_tx_lock == NULL) return ESP_ERR_INVALID_STATE;
    if (data == NULL || length == 0U) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int written = uart_write_bytes(
        (uart_port_t)BOARD_WT99_VOICE_UART_PORT, data, length);
    xSemaphoreGive(s_tx_lock);
    return written == (int)length ? ESP_OK : ESP_FAIL;
}

static esp_err_t send_audio_feedback(uint8_t type, uint16_t sequence)
{
    uint8_t wire[S3_VOICE_LINK_WIRE_OVERHEAD_BYTES];
    size_t wire_length = 0;
    esp_err_t err = s3_voice_link_encode_packet(
        type, 0, sequence, NULL, 0, wire, sizeof(wire), &wire_length);
    return err == ESP_OK ? write_locked(wire, wire_length) : err;
}

static void dispatch_event(s3_voice_event_type_t type, uint8_t command_id)
{
    if (s_handler == NULL) return;
    const s3_voice_event_t event = {
        .type = type,
        .command_id = command_id,
    };
    s_handler(&event, s_handler_context);
}

static void handle_line(char *line)
{
    if (strcmp(line, "V1,WAKE") == 0) {
        s_expected_audio_sequence = 0;
        s_expect_audio = true;
        s_drop_audio_until_stop = false;
        reset_idle_audio_resync();
        dispatch_event(S3_VOICE_EVENT_WAKE, 0);
        return;
    }
    if (strcmp(line, "V1,TIMEOUT") == 0) {
        dispatch_event(S3_VOICE_EVENT_TIMEOUT, 0);
        return;
    }
    if (strcmp(line, "V2,AI,BEGIN") == 0) {
        dispatch_event(S3_VOICE_EVENT_AI_BEGIN, 0);
        return;
    }
    if (strcmp(line, "V2,AI,SPEECH_END") == 0) {
        /* This marker is queued behind the last S3 PCM frame. Keep ACKing
         * duplicate tail frames, but never forward more PCM to the cloud. */
        s_drop_audio_until_stop = true;
        dispatch_event(S3_VOICE_EVENT_SPEECH_END, 0);
        return;
    }
    if (strcmp(line, "V2,AI,CANCEL") == 0) {
        s_expect_audio = false;
        s_drop_audio_until_stop = false;
        dispatch_event(S3_VOICE_EVENT_AI_CANCEL, 0);
        return;
    }

    static const char command_prefix[] = "V1,CMD,";
    if (strncmp(line, command_prefix, sizeof(command_prefix) - 1U) != 0) {
        ESP_LOGW(TAG, "ignored malformed frame: %s", line);
        return;
    }

    char *end = NULL;
    long command_id = strtol(line + sizeof(command_prefix) - 1U, &end, 10);
    if (end == NULL || *end != '\0' ||
        command_id < VOICE_COMMAND_ID_MIN ||
        command_id > VOICE_COMMAND_ID_MAX) {
        ESP_LOGW(TAG, "ignored invalid command frame: %s", line);
        return;
    }
    s_expect_audio = false;
    s_drop_audio_until_stop = false;
    dispatch_event(S3_VOICE_EVENT_COMMAND, (uint8_t)command_id);
}

static bool sequence_before(uint16_t left, uint16_t right)
{
    return (int16_t)(left - right) < 0;
}

static void handle_binary_packet(const s3_voice_link_packet_t *packet)
{
    if (packet == NULL || packet->type != S3_VOICE_LINK_PACKET_AUDIO) {
        ESP_LOGW(TAG, "ignored unexpected binary packet type=%u",
                 packet != NULL ? packet->type : 0U);
        return;
    }
    if (!s_expect_audio) {
        /* P4 may restart while S3 still owns an old streaming session. ACK
         * every stale packet so the S3 replay/tail window can drain, and
         * periodically repeat STOP until both ends agree that the route is
         * idle. Never log each frame: that flood can starve the UART RX and
         * voice tasks and makes a recoverable desynchronisation look like a
         * crash. */
        (void)send_audio_feedback(S3_VOICE_LINK_PACKET_ACK,
                                  packet->sequence);
        ++s_idle_discarded_frames;

        const TickType_t now = xTaskGetTickCount();
        const TickType_t interval =
            pdMS_TO_TICKS(VOICE_IDLE_RESYNC_INTERVAL_MS);
        if (s_idle_last_resync_tick == 0 ||
            (now - s_idle_last_resync_tick) >= interval) {
            esp_err_t stop_error = s3_voice_node_send_ai_stop("IDLE_RESYNC");
            if (stop_error == ESP_OK) {
                ESP_LOGW(TAG,
                         "idle UART audio resync: discarded=%u last_seq=%u; STOP re-sent",
                         (unsigned)s_idle_discarded_frames,
                         (unsigned)packet->sequence);
            } else {
                ESP_LOGW(TAG,
                         "idle UART audio resync failed: discarded=%u last_seq=%u error=%s",
                         (unsigned)s_idle_discarded_frames,
                         (unsigned)packet->sequence,
                         esp_err_to_name(stop_error));
            }
            s_idle_discarded_frames = 0;
            s_idle_last_resync_tick = now;
        }
        return;
    }
    if (packet->sequence != s_expected_audio_sequence) {
        if (sequence_before(packet->sequence, s_expected_audio_sequence)) {
            (void)send_audio_feedback(S3_VOICE_LINK_PACKET_ACK,
                                      (uint16_t)(s_expected_audio_sequence - 1U));
        } else {
            ESP_LOGW(TAG, "UART audio gap: expected=%u received=%u",
                     (unsigned)s_expected_audio_sequence,
                     (unsigned)packet->sequence);
            (void)send_audio_feedback(S3_VOICE_LINK_PACKET_NACK,
                                      s_expected_audio_sequence);
        }
        return;
    }

    if (s_drop_audio_until_stop) {
        ++s_expected_audio_sequence;
        (void)send_audio_feedback(S3_VOICE_LINK_PACKET_ACK,
                                  packet->sequence);
        return;
    }

    int16_t pcm[VOICE_PCM_FRAME_SAMPLES];
    size_t sample_count = 0;
    esp_err_t err = s3_voice_adpcm_decode(
        packet->payload, packet->payload_length,
        pcm, sizeof(pcm) / sizeof(pcm[0]), &sample_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "invalid ADPCM seq=%u: %s",
                 (unsigned)packet->sequence, esp_err_to_name(err));
        (void)send_audio_feedback(S3_VOICE_LINK_PACKET_NACK,
                                  s_expected_audio_sequence);
        return;
    }

    if (s_audio_handler != NULL) {
        err = s_audio_handler(pcm, sample_count, packet->sequence,
                              s_audio_context);
        if (err != ESP_OK) {
            if (err == ESP_ERR_INVALID_STATE) {
                /* The AI route has just closed.  ACK and discard queued tail
                 * frames until AI_STOP arrives; a NACK here creates a replay
                 * storm that can delay the higher-priority stop control line. */
                s_drop_audio_until_stop = true;
                ++s_expected_audio_sequence;
                (void)send_audio_feedback(S3_VOICE_LINK_PACKET_ACK,
                                          packet->sequence);
                ESP_LOGI(TAG,
                         "AI audio route closed at seq=%u; draining UART tail",
                         (unsigned)packet->sequence);
                return;
            }
            ESP_LOGW(TAG, "audio consumer rejected seq=%u: %s",
                     (unsigned)packet->sequence, esp_err_to_name(err));
            (void)send_audio_feedback(S3_VOICE_LINK_PACKET_NACK,
                                      s_expected_audio_sequence);
            return;
        }
    }
    ++s_expected_audio_sequence;
    (void)send_audio_feedback(S3_VOICE_LINK_PACKET_ACK,
                              packet->sequence);
    if ((packet->sequence % 50U) == 49U) {
        ESP_LOGI(TAG, "UART audio received: seq=%u samples=%u",
                 (unsigned)packet->sequence, (unsigned)sample_count);
    }
}

static void voice_rx_task(void *argument)
{
    (void)argument;
    uint8_t rx[64];
    char line[VOICE_LINE_BUFFER_BYTES];
    size_t line_length = 0;
    bool discarding = false;
    bool possible_binary = false;
    bool binary = false;
    s3_voice_link_parser_t parser;

    while (true) {
        int received = uart_read_bytes(
            (uart_port_t)BOARD_WT99_VOICE_UART_PORT, rx, sizeof(rx),
            pdMS_TO_TICKS(100));
        if (received <= 0) continue;

        for (int index = 0; index < received; ++index) {
            const uint8_t raw = rx[index];
            if (binary) {
                s3_voice_link_packet_t packet;
                s3_voice_link_parse_result_t parse =
                    s3_voice_link_parser_feed(&parser, raw, &packet);
                if (parse == S3_VOICE_LINK_PARSE_COMPLETE) {
                    handle_binary_packet(&packet);
                    binary = false;
                } else if (parse == S3_VOICE_LINK_PARSE_ERROR) {
                    ESP_LOGW(TAG, "corrupt UART audio frame; requesting seq=%u",
                             (unsigned)s_expected_audio_sequence);
                    if (s_expect_audio) {
                        (void)send_audio_feedback(
                            S3_VOICE_LINK_PACKET_NACK,
                            s_expected_audio_sequence);
                    }
                    binary = false;
                }
                continue;
            }
            if (possible_binary) {
                possible_binary = false;
                if (raw == S3_VOICE_LINK_MAGIC_1) {
                    s3_voice_link_parser_begin(&parser);
                    binary = true;
                    line_length = 0;
                    discarding = false;
                    continue;
                }
                if (!discarding && line_length + 1U < sizeof(line)) {
                    line[line_length++] = (char)S3_VOICE_LINK_MAGIC_0;
                }
            }
            if (raw == S3_VOICE_LINK_MAGIC_0) {
                possible_binary = true;
                continue;
            }

            char byte = (char)raw;
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
                ESP_LOGW(TAG, "oversized voice frame discarded");
                discarding = true;
                line_length = 0;
            }
        }
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
        VOICE_RX_BUFFER_BYTES, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(
            (uart_port_t)BOARD_WT99_VOICE_UART_PORT, &config);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(
            (uart_port_t)BOARD_WT99_VOICE_UART_PORT,
            BOARD_WT99_VOICE_UART_TX_GPIO,
            BOARD_WT99_VOICE_UART_RX_GPIO,
            UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        uart_driver_delete((uart_port_t)BOARD_WT99_VOICE_UART_PORT);
        return err;
    }

    s_tx_lock = xSemaphoreCreateMutex();
    if (s_tx_lock == NULL) {
        uart_driver_delete((uart_port_t)BOARD_WT99_VOICE_UART_PORT);
        return ESP_ERR_NO_MEM;
    }
    s_handler = handler;
    s_handler_context = context;
    uart_flush_input((uart_port_t)BOARD_WT99_VOICE_UART_PORT);
    s_initialized = true;
    if (xTaskCreate(voice_rx_task, "voice_s3_rx",
                    VOICE_RX_TASK_STACK_BYTES, NULL,
                    VOICE_RX_TASK_PRIORITY, NULL) != pdPASS) {
        s_initialized = false;
        vSemaphoreDelete(s_tx_lock);
        s_tx_lock = NULL;
        s_handler = NULL;
        s_handler_context = NULL;
        uart_driver_delete((uart_port_t)BOARD_WT99_VOICE_UART_PORT);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "UART%d TX=GPIO%d RX=GPIO%d at %d baud",
             BOARD_WT99_VOICE_UART_PORT,
             BOARD_WT99_VOICE_UART_TX_GPIO,
             BOARD_WT99_VOICE_UART_RX_GPIO,
             BOARD_WT99_VOICE_UART_BAUD_RATE);
    return ESP_OK;
}

esp_err_t s3_voice_node_send_ack(uint8_t command_id, bool handled)
{
    if (!s_initialized || s_tx_lock == NULL) return ESP_ERR_INVALID_STATE;
    if (command_id < VOICE_COMMAND_ID_MIN ||
        command_id > VOICE_COMMAND_ID_MAX) return ESP_ERR_INVALID_ARG;

    char line[32];
    int length = snprintf(line, sizeof(line), "V1,ACK,%u,%s\n",
                          (unsigned)command_id,
                          handled ? "OK" : "IGNORED");
    if (length <= 0 || length >= (int)sizeof(line)) return ESP_FAIL;

    return write_locked(line, (size_t)length);
}

void s3_voice_node_set_audio_handler(s3_voice_audio_handler_t handler,
                                     void *context)
{
    s_audio_handler = handler;
    s_audio_context = context;
}

esp_err_t s3_voice_node_send_ai_stop(const char *reason)
{
    const char *safe_reason = reason != NULL ? reason : "DONE";
    char line[48];
    int length = snprintf(line, sizeof(line), "V2,AI,STOP,%.24s\n",
                          safe_reason);
    if (length <= 0 || length >= (int)sizeof(line)) return ESP_FAIL;
    s_expect_audio = false;
    s_drop_audio_until_stop = false;
    return write_locked(line, (size_t)length);
}

esp_err_t s3_voice_node_send_ai_input_done(void)
{
    s_drop_audio_until_stop = true;
    return write_locked("V2,AI,INPUT_DONE\n",
                        sizeof("V2,AI,INPUT_DONE\n") - 1U);
}
