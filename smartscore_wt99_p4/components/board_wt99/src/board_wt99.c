#include "board_wt99.h"

#include "board_wt99_pins.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "BOARD";
static const char *s_block_reason = "WT99 Hosted configuration not checked";

static bool transport_configuration_matches(void)
{
#if !CONFIG_SMARTSCORE_WT99_BOARD_REV_1V1
    s_block_reason = "WT99P4C5-S1 revision 1V1 is not selected";
    return false;
#elif !CONFIG_SLAVE_IDF_TARGET_ESP32C5 || !CONFIG_ESP_HOSTED_CP_TARGET_ESP32C5
    s_block_reason = "ESP-Hosted co-processor target is not ESP32-C5";
    return false;
#elif !CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE
    s_block_reason = "ESP-Hosted transport is not SDIO";
    return false;
#elif !CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH
    s_block_reason = "C5 EN reset sequence must be high-low-high";
    return false;
#elif !CONFIG_ESP_HOSTED_SDIO_SLOT_1 || CONFIG_ESP_HOSTED_SDIO_SLOT != BOARD_WT99_C5_SDIO_SLOT
    s_block_reason = "ESP-Hosted must use SDIO slot 1";
    return false;
#elif !CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS || CONFIG_ESP_HOSTED_SDIO_BUS_WIDTH != BOARD_WT99_C5_SDIO_BUS_WIDTH
    s_block_reason = "ESP-Hosted must use the 4-bit SDIO bus";
    return false;
#elif CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ != BOARD_WT99_C5_SDIO_CLOCK_KHZ
    s_block_reason = "ESP-Hosted SDIO clock must be 40 MHz";
    return false;
#elif CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D0_SLOT_1 != BOARD_WT99_C5_SDIO_D0_GPIO || \
      CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D1_4BIT_BUS_SLOT_1 != BOARD_WT99_C5_SDIO_D1_GPIO || \
      CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D2_4BIT_BUS_SLOT_1 != BOARD_WT99_C5_SDIO_D2_GPIO || \
      CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D3_4BIT_BUS_SLOT_1 != BOARD_WT99_C5_SDIO_D3_GPIO || \
      CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CLK_SLOT_1 != BOARD_WT99_C5_SDIO_CLK_GPIO || \
      CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CMD_SLOT_1 != BOARD_WT99_C5_SDIO_CMD_GPIO
    s_block_reason = "ESP-Hosted SDIO GPIOs do not match WT99 revision 1V1";
    return false;
#elif CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE != BOARD_WT99_C5_RESET_GPIO
    s_block_reason = "ESP-Hosted C5 reset GPIO must be GPIO54";
    return false;
#else
    s_block_reason = "none";
    return true;
#endif
}

bool board_wt99_network_supported(void)
{
    return transport_configuration_matches();
}

const char *board_wt99_network_block_reason(void)
{
    return s_block_reason;
}

esp_err_t board_wt99_network_power_enable(void)
{
    if (!transport_configuration_matches()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* U14 supplies the C5 3.3 V rail whenever the board 5 V rail is on. */
    ESP_LOGI(TAG, "C5 uses the board's always-on 3.3 V rail");
    return ESP_OK;
}

esp_err_t board_wt99_c5_reset(void)
{
    if (!transport_configuration_matches()) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "C5 reset GPIO=%d; reset pulse owned by ESP-Hosted",
             BOARD_WT99_C5_RESET_GPIO);
    return ESP_OK;
}

esp_err_t board_wt99_c5_transport_prepare(void)
{
    if (!transport_configuration_matches()) {
        ESP_LOGE(TAG, "Hosted configuration rejected: %s", s_block_reason);
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG,
             "C5 SDIO prepared: slot=%d width=%d clock=%d kHz CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d reset=%d",
             BOARD_WT99_C5_SDIO_SLOT,
             BOARD_WT99_C5_SDIO_BUS_WIDTH,
             BOARD_WT99_C5_SDIO_CLOCK_KHZ,
             BOARD_WT99_C5_SDIO_CLK_GPIO,
             BOARD_WT99_C5_SDIO_CMD_GPIO,
             BOARD_WT99_C5_SDIO_D0_GPIO,
             BOARD_WT99_C5_SDIO_D1_GPIO,
             BOARD_WT99_C5_SDIO_D2_GPIO,
             BOARD_WT99_C5_SDIO_D3_GPIO,
             BOARD_WT99_C5_RESET_GPIO);
    return ESP_OK;
}
