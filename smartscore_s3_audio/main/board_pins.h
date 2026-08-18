#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"

/* V2 wiring. All pin values are ESP32-S3 GPIO numbers, not module pad numbers. */
#define BOARD_I2C_PORT                 I2C_NUM_0
#define BOARD_I2C_SDA_GPIO             GPIO_NUM_21
#define BOARD_I2C_SCL_GPIO             GPIO_NUM_14
#define BOARD_I2C_SPEED_HZ             400000
#define BOARD_ES7210_I2C_ADDR_7BIT     0x40

#define BOARD_I2S_PORT                 I2S_NUM_0
#define BOARD_I2S_MCLK_GPIO            GPIO_NUM_9
#define BOARD_I2S_BCLK_GPIO            GPIO_NUM_13
#define BOARD_I2S_WS_GPIO              GPIO_NUM_12
#define BOARD_I2S_DIN_GPIO             GPIO_NUM_11
#define BOARD_ES7210_INT_GPIO          GPIO_NUM_10

/* Dedicated S3 <-> external main-controller music link.
 * PCB netlist: H7.8/U2 module pad 38 = GPIO2 (MUSIC_RX_MS, S3 RX),
 * H7.10/U2 module pad 39 = GPIO1 (MUSIC_TX_MS, S3 TX). */
#define BOARD_MUSIC_LINK_UART_PORT     UART_NUM_1
#define BOARD_MUSIC_LINK_TX_GPIO       GPIO_NUM_1
#define BOARD_MUSIC_LINK_RX_GPIO       GPIO_NUM_2
#define BOARD_MUSIC_LINK_BAUD_RATE     115200

#define BOARD_AUDIO_SAMPLE_RATE_HZ     24000
#define BOARD_AUDIO_CHANNELS           2
#define BOARD_AUDIO_EFFECTIVE_BITS     16
#define BOARD_AUDIO_BITS_PER_SLOT      16
#define BOARD_AUDIO_SLOT_COUNT         2
#define BOARD_AUDIO_MCLK_MULTIPLE      256

/* Nominal SDOUT1 layout for ES7210 MIC1+MIC2 in standard I2S mode.
 * In single-CN1 mode the capture layer verifies the live slot before DSP. */
#define BOARD_MIC1_SLOT_INDEX          0
#define BOARD_MIC2_SLOT_INDEX          1
#define BOARD_AUTO_DETECT_MIC1_SLOT    1

#define BOARD_ENABLE_CHANNEL_DIAGNOSTICS 1
#define BOARD_ENABLE_PERFORMANCE_LOGS    1
