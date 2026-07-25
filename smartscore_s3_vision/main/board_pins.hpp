#pragma once

#include "driver/gpio.h"

namespace board {

inline constexpr gpio_num_t CAM_PIN_PWDN = GPIO_NUM_14;
inline constexpr gpio_num_t CAM_PIN_RESET = GPIO_NUM_41;

inline constexpr gpio_num_t CAM_PIN_XCLK = GPIO_NUM_15;
inline constexpr gpio_num_t CAM_PIN_SIOD = GPIO_NUM_4;
inline constexpr gpio_num_t CAM_PIN_SIOC = GPIO_NUM_5;

inline constexpr gpio_num_t CAM_PIN_D0 = GPIO_NUM_11;
inline constexpr gpio_num_t CAM_PIN_D1 = GPIO_NUM_9;
inline constexpr gpio_num_t CAM_PIN_D2 = GPIO_NUM_8;
inline constexpr gpio_num_t CAM_PIN_D3 = GPIO_NUM_10;
inline constexpr gpio_num_t CAM_PIN_D4 = GPIO_NUM_12;
inline constexpr gpio_num_t CAM_PIN_D5 = GPIO_NUM_18;
inline constexpr gpio_num_t CAM_PIN_D6 = GPIO_NUM_17;
inline constexpr gpio_num_t CAM_PIN_D7 = GPIO_NUM_16;

inline constexpr gpio_num_t CAM_PIN_VSYNC = GPIO_NUM_6;
inline constexpr gpio_num_t CAM_PIN_HREF = GPIO_NUM_7;
inline constexpr gpio_num_t CAM_PIN_PCLK = GPIO_NUM_13;

inline constexpr gpio_num_t SDMMC_PIN_CMD = GPIO_NUM_38;
inline constexpr gpio_num_t SDMMC_PIN_CLK = GPIO_NUM_39;
inline constexpr gpio_num_t SDMMC_PIN_D0 = GPIO_NUM_40;

// Dedicated control link to WT99 P4 UART3.
// S3 GPIO1/TX -> H7-17 CAM_TX_MS -> P4 J6-27 GPIO48/RX.
// P4 J6-25 GPIO47/TX -> H7-15 CAM_RX_MS -> S3 GPIO2/RX.
inline constexpr int P4_LINK_UART_PORT = 1;
inline constexpr gpio_num_t P4_LINK_UART_TX = GPIO_NUM_1;
inline constexpr gpio_num_t P4_LINK_UART_RX = GPIO_NUM_2;
inline constexpr int P4_LINK_UART_BAUD_RATE = 115200;

} // namespace board
