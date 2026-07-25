#include "board_audio.h"

#include <string.h>

#include "audio_codec_ctrl_if.h"
#include "audio_codec_data_if.h"
#include "audio_codec_gpio_if.h"
#include "audio_codec_if.h"
#include "board_wt99_pins.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

static const char *TAG = "BOARD_AUDIO";

static i2c_master_bus_handle_t s_i2c_bus;
static i2s_chan_handle_t s_i2s_tx;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_if_t *s_codec_if;
static esp_codec_dev_handle_t s_codec;
static board_audio_status_t s_status = {
    .volume_percent = BOARD_WT99_AUDIO_INITIAL_VOLUME_PERCENT,
    .sample_rate_hz = BOARD_WT99_AUDIO_DEFAULT_SAMPLE_RATE_HZ,
};

static esp_err_t amp_set_enabled(bool enabled)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOARD_WT99_AUDIO_PA_ENABLE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err == ESP_OK) {
        int level = enabled ? BOARD_WT99_AUDIO_PA_ENABLE_LEVEL
                            : !BOARD_WT99_AUDIO_PA_ENABLE_LEVEL;
        err = gpio_set_level(BOARD_WT99_AUDIO_PA_ENABLE_GPIO, level);
    }
    if (err == ESP_OK) {
        s_status.amp_enabled = enabled;
    }
    return err;
}

static void reset_status(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.volume_percent = BOARD_WT99_AUDIO_INITIAL_VOLUME_PERCENT;
    s_status.sample_rate_hz = BOARD_WT99_AUDIO_DEFAULT_SAMPLE_RATE_HZ;
}

static void cleanup_partial(void)
{
    if (s_codec != NULL) {
        esp_codec_dev_close(s_codec);
        esp_codec_dev_delete(s_codec);
        s_codec = NULL;
    }
    if (s_codec_if != NULL) {
        audio_codec_delete_codec_if(s_codec_if);
        s_codec_if = NULL;
    }
    if (s_ctrl_if != NULL) {
        audio_codec_delete_ctrl_if(s_ctrl_if);
        s_ctrl_if = NULL;
    }
    if (s_gpio_if != NULL) {
        audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
    }
    if (s_data_if != NULL) {
        audio_codec_delete_data_if(s_data_if);
        s_data_if = NULL;
    }
    if (s_i2s_tx != NULL) {
        i2s_channel_disable(s_i2s_tx);
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
    }
    if (s_i2c_bus != NULL) {
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
    }
    amp_set_enabled(false);
    reset_status();
}

esp_err_t board_audio_init(void)
{
    if (s_status.initialized) {
        return ESP_OK;
    }

    esp_err_t err = amp_set_enabled(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to force NS4150B off: %s", esp_err_to_name(err));
        return err;
    }

    i2c_master_bus_config_t i2c_config = {
        .i2c_port = BOARD_WT99_AUDIO_I2C_PORT,
        .sda_io_num = BOARD_WT99_AUDIO_I2C_SDA_GPIO,
        .scl_io_num = BOARD_WT99_AUDIO_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };
    err = i2c_new_master_bus(&i2c_config, &s_i2c_bus);
    if (err != ESP_OK) {
        cleanup_partial();
        return err;
    }

    err = i2c_master_probe(s_i2c_bus,
                           BOARD_WT99_ES8311_I2C_ADDRESS_7BIT,
                           100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 did not ACK at 0x%02X: %s",
                 BOARD_WT99_ES8311_I2C_ADDRESS_7BIT,
                 esp_err_to_name(err));
        cleanup_partial();
        return err;
    }

    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(BOARD_WT99_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = 256;
    err = i2s_new_channel(&channel_config, &s_i2s_tx, NULL);
    if (err != ESP_OK) {
        cleanup_partial();
        return err;
    }

    i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(
            BOARD_WT99_AUDIO_DEFAULT_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BOARD_WT99_AUDIO_I2S_MCLK_GPIO,
            .bclk = BOARD_WT99_AUDIO_I2S_BCLK_GPIO,
            .ws = BOARD_WT99_AUDIO_I2S_LRCLK_GPIO,
            .dout = BOARD_WT99_AUDIO_I2S_DOUT_GPIO,
            .din = BOARD_WT99_AUDIO_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    err = i2s_channel_init_std_mode(s_i2s_tx, &i2s_config);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_i2s_tx);
    }
    if (err != ESP_OK) {
        cleanup_partial();
        return err;
    }

    audio_codec_i2s_cfg_t data_config = {
        .port = BOARD_WT99_AUDIO_I2S_PORT,
        .tx_handle = s_i2s_tx,
        .rx_handle = NULL,
    };
    s_data_if = audio_codec_new_i2s_data(&data_config);
    s_gpio_if = audio_codec_new_gpio();

    audio_codec_i2c_cfg_t control_config = {
        .port = BOARD_WT99_AUDIO_I2C_PORT,
        .addr = BOARD_WT99_ES8311_CODEC_DEV_ADDRESS,
        .bus_handle = s_i2c_bus,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&control_config);
    if (s_data_if == NULL || s_gpio_if == NULL || s_ctrl_if == NULL) {
        cleanup_partial();
        return ESP_ERR_NO_MEM;
    }

    esp_codec_dev_hw_gain_t hardware_gain = {
        .pa_voltage = 5.0f,
        .codec_dac_voltage = 3.3f,
    };
    es8311_codec_cfg_t codec_config = {
        .ctrl_if = s_ctrl_if,
        .gpio_if = s_gpio_if,
        .codec_mode = ESP_CODEC_DEV_TYPE_OUT,
        .pa_pin = BOARD_WT99_AUDIO_PA_ENABLE_GPIO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = hardware_gain,
    };
    s_codec_if = es8311_codec_new(&codec_config);
    if (s_codec_if == NULL) {
        cleanup_partial();
        return ESP_ERR_NO_MEM;
    }

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
    };
    s_codec = esp_codec_dev_new(&device_config);
    if (s_codec == NULL) {
        cleanup_partial();
        return ESP_ERR_NO_MEM;
    }

    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = BOARD_WT99_AUDIO_DEFAULT_SAMPLE_RATE_HZ,
        .channel = 1,
        .bits_per_sample = 16,
    };
    err = esp_codec_dev_open(s_codec, &sample_info);
    if (err == ESP_OK) {
        err = esp_codec_dev_set_out_vol(
            s_codec, BOARD_WT99_AUDIO_INITIAL_VOLUME_PERCENT);
    }
    if (err == ESP_OK) {
        err = esp_codec_dev_set_out_mute(s_codec, true);
    }
    if (err == ESP_OK) {
        err = amp_set_enabled(false);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 open failed: %s", esp_err_to_name(err));
        cleanup_partial();
        return err;
    }

    s_status.initialized = true;
    s_status.codec_ready = true;
    s_status.muted = false;
    s_status.volume_percent = BOARD_WT99_AUDIO_INITIAL_VOLUME_PERCENT;
    s_status.sample_rate_hz = BOARD_WT99_AUDIO_DEFAULT_SAMPLE_RATE_HZ;
    s_status.last_error = ESP_OK;
    ESP_LOGI(TAG,
             "ES8311 ready: I2C1 SDA=%d SCL=%d, I2S1 24kHz mono, PA GPIO=%d off, volume=%u%%",
             BOARD_WT99_AUDIO_I2C_SDA_GPIO,
             BOARD_WT99_AUDIO_I2C_SCL_GPIO,
             BOARD_WT99_AUDIO_PA_ENABLE_GPIO,
             s_status.volume_percent);
    return ESP_OK;
}

esp_err_t board_audio_deinit(void)
{
    cleanup_partial();
    return ESP_OK;
}

esp_err_t board_audio_force_disabled(void)
{
    if (s_status.initialized) {
        cleanup_partial();
        return ESP_OK;
    }

    esp_err_t err = amp_set_enabled(false);
    if (err == ESP_OK) {
        reset_status();
    }
    return err;
}

esp_err_t board_audio_begin_output(void)
{
    if (!s_status.initialized || s_codec == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_status.output_active) {
        return ESP_OK;
    }

    esp_err_t err = esp_codec_dev_set_out_mute(s_codec, s_status.muted);
    if (err == ESP_OK) {
        err = amp_set_enabled(!s_status.muted);
    }
    if (err == ESP_OK) {
        s_status.output_active = true;
    } else {
        esp_codec_dev_set_out_mute(s_codec, true);
        amp_set_enabled(false);
    }
    s_status.last_error = err;
    return err;
}

esp_err_t board_audio_end_output(void)
{
    if (!s_status.initialized || s_codec == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_status.output_active) {
        return amp_set_enabled(false);
    }

    int16_t silence[64] = {0};
    esp_codec_dev_write(s_codec, silence, sizeof(silence));
    esp_err_t err = esp_codec_dev_set_out_mute(s_codec, true);
    esp_err_t amp_err = amp_set_enabled(false);
    s_status.output_active = false;
    if (err == ESP_OK) {
        err = amp_err;
    }
    s_status.last_error = err;
    return err;
}

esp_err_t board_audio_write(const int16_t *samples, size_t sample_count)
{
    if (!s_status.initialized || !s_status.output_active ||
        s_codec == NULL || samples == NULL || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_codec_dev_write(
        s_codec, (void *)samples, sample_count * sizeof(int16_t));
    if (err != ESP_OK) {
        s_status.write_errors++;
        s_status.last_error = err;
    }
    return err;
}

esp_err_t board_audio_set_volume(uint8_t percent)
{
    if (!s_status.initialized || s_codec == NULL ||
        percent > BOARD_WT99_AUDIO_MAX_VOLUME_PERCENT) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_codec_dev_set_out_vol(s_codec, percent);
    if (err == ESP_OK) {
        s_status.volume_percent = percent;
    }
    s_status.last_error = err;
    return err;
}

esp_err_t board_audio_set_mute(bool muted)
{
    if (!s_status.initialized || s_codec == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    if (s_status.output_active || muted) {
        err = esp_codec_dev_set_out_mute(s_codec, muted);
    }
    if (err == ESP_OK && s_status.output_active) {
        err = amp_set_enabled(!muted);
    } else if (muted) {
        amp_set_enabled(false);
    }
    if (err == ESP_OK) {
        s_status.muted = muted;
    }
    s_status.last_error = err;
    return err;
}

esp_err_t board_audio_set_sample_rate(uint32_t sample_rate_hz)
{
    if (!s_status.initialized || s_codec == NULL ||
        (sample_rate_hz != 16000 && sample_rate_hz != 24000)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_rate_hz == s_status.sample_rate_hz) {
        return ESP_OK;
    }

    esp_err_t err = board_audio_end_output();
    if (err == ESP_OK) {
        err = esp_codec_dev_close(s_codec);
    }
    if (err == ESP_OK) {
        err = i2s_channel_disable(s_i2s_tx);
    }
    i2s_std_clk_config_t clock = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
    if (err == ESP_OK) {
        err = i2s_channel_reconfig_std_clock(s_i2s_tx, &clock);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_i2s_tx);
    }

    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = sample_rate_hz,
        .channel = 1,
        .bits_per_sample = 16,
    };
    if (err == ESP_OK) {
        err = esp_codec_dev_open(s_codec, &sample_info);
    }
    if (err == ESP_OK) {
        err = esp_codec_dev_set_out_vol(s_codec, s_status.volume_percent);
    }
    if (err == ESP_OK) {
        err = esp_codec_dev_set_out_mute(s_codec, true);
    }
    if (err == ESP_OK) {
        s_status.sample_rate_hz = sample_rate_hz;
        s_status.output_active = false;
        s_status.amp_enabled = false;
    }
    s_status.last_error = err;
    return err;
}

void board_audio_get_status(board_audio_status_t *out_status)
{
    if (out_status != NULL) {
        *out_status = s_status;
    }
}
