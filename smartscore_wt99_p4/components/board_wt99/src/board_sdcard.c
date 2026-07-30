#include "board_sdcard.h"

#include <string.h>

#include "board_wt99_pins.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

#define BOARD_SDCARD_LDO_CHANNEL 4
#define BOARD_SDCARD_SAFE_CLOCK_KHZ 10000

static const char *TAG = "BOARD_SD";
static board_sdcard_status_t s_status;
static sdmmc_card_t *s_card;
static sd_pwr_ctrl_handle_t s_power;

esp_err_t board_sdcard_mount(void)
{
    if (s_status.mounted) {
        return ESP_OK;
    }

    sd_pwr_ctrl_ldo_config_t power_config = {
        .ldo_chan_id = BOARD_SDCARD_LDO_CHANNEL,
    };
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&power_config, &s_power);
    if (err != ESP_OK) {
        s_status.last_error = err;
        ESP_LOGW(TAG, "SD power LDO unavailable: %s", esp_err_to_name(err));
        return err;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.pwr_ctrl_handle = s_power;
    host.max_freq_khz = BOARD_SDCARD_SAFE_CLOCK_KHZ;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    /* Audio streaming needs very little bandwidth. 1-bit mode plus internal
     * pull-ups is substantially more tolerant of long display-board traces
     * and cards whose D1..D3 lines are slow during power-up. */
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    slot.clk = BOARD_WT99_SDMMC_CLK_GPIO;
    slot.cmd = BOARD_WT99_SDMMC_CMD_GPIO;
    slot.d0 = BOARD_WT99_SDMMC_D0_GPIO;
    slot.d1 = BOARD_WT99_SDMMC_D1_GPIO;
    slot.d2 = BOARD_WT99_SDMMC_D2_GPIO;
    slot.d3 = BOARD_WT99_SDMMC_D3_GPIO;
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
    };
    err = esp_vfs_fat_sdmmc_mount(BOARD_SDCARD_MOUNT_POINT,
                                  &host,
                                  &slot,
                                  &mount_config,
                                  &s_card);
    if (err != ESP_OK) {
        s_status.last_error = err;
        sd_pwr_ctrl_del_on_chip_ldo(s_power);
        s_power = NULL;
        ESP_LOGW(TAG, "SD card not mounted: %s", esp_err_to_name(err));
        return err;
    }

    s_status.mounted = true;
    s_status.capacity_bytes =
        (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    s_status.last_error = ESP_OK;
    ESP_LOGI(TAG,
             "SD mounted on slot 0, 1-bit D0=%d CLK=%d CMD=%d at %dkHz, capacity=%llu",
             BOARD_WT99_SDMMC_D0_GPIO,
             BOARD_WT99_SDMMC_CLK_GPIO,
             BOARD_WT99_SDMMC_CMD_GPIO,
             BOARD_SDCARD_SAFE_CLOCK_KHZ,
             (unsigned long long)s_status.capacity_bytes);
    return ESP_OK;
}

void board_sdcard_get_status(board_sdcard_status_t *out_status)
{
    if (out_status != NULL) {
        *out_status = s_status;
    }
}
