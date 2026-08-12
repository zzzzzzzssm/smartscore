#pragma once

#include "driver/i2s_std.h"
#include "driver/uart.h"

#define VOICE_I2S_PORT                  I2S_NUM_0
#define VOICE_I2S_BCLK_GPIO             GPIO_NUM_10
#define VOICE_I2S_WS_GPIO               GPIO_NUM_11
#define VOICE_I2S_DIN_GPIO              GPIO_NUM_9
#define VOICE_SAMPLE_RATE_HZ            16000U
#define VOICE_I2S_READ_TIMEOUT_MS       1000U

/* INMP441 sends 24-bit samples left-aligned in the 32-bit I2S slot. */
#define VOICE_MIC_DIGITAL_GAIN          4

#define VOICE_EXPECTED_FLASH_BYTES      (16U * 1024U * 1024U)
#define VOICE_EXPECTED_PSRAM_BYTES      (8U * 1024U * 1024U)

#define VOICE_MODEL_PARTITION_LABEL     "model"
#define VOICE_WAKE_MODEL_KEYWORD        "nihaoxiaozhi"
#define VOICE_WAKE_WORD_TEXT            "你好小智"
#define VOICE_COMMAND_TIMEOUT_MS        6000

/* Single-turn AI routing keeps the original MultiNet timeout as a fallback. */
#define VOICE_AI_POST_WAKE_GUARD_MS       80U
#define VOICE_AI_MIN_QUERY_SPEECH_MS     256U
#define VOICE_AI_END_SILENCE_MS          500U

/* H7 voice link: GPIO1/TX -> VOICE_TX_MS, GPIO2/RX <- VOICE_RX_MS. */
#define VOICE_LINK_UART_PORT            UART_NUM_1
#define VOICE_LINK_TX_GPIO              GPIO_NUM_1
#define VOICE_LINK_RX_GPIO              GPIO_NUM_2
#define VOICE_LINK_BAUD_RATE            115200

#define VOICE_TASK_PRIORITY             5
#define VOICE_FEED_TASK_STACK_SIZE      (6U * 1024U)
#define VOICE_DETECT_TASK_STACK_SIZE    (8U * 1024U)
#define VOICE_FEED_TASK_CORE            0
#define VOICE_DETECT_TASK_CORE          1

typedef enum {
    VOICE_COMMAND_START_PRACTICE = 1,
    VOICE_COMMAND_STOP_PRACTICE = 2,
    VOICE_COMMAND_NEXT_PAGE = 3,
    VOICE_COMMAND_PREVIOUS_PAGE = 4,
    VOICE_COMMAND_RESTART = 5,
    VOICE_COMMAND_VIEW_SCORE = 6,
    VOICE_COMMAND_HOME = 7,
} voice_command_id_t;

#define VOICE_COMMAND_ID_MIN VOICE_COMMAND_START_PRACTICE
#define VOICE_COMMAND_ID_MAX VOICE_COMMAND_HOME
