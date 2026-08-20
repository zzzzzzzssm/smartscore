#include "es7210_capture.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "audio_codec_ctrl_if.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "es7210_adc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "board_pins.h"
#include "diagnostics.h"

#define ES7210_REG_CHIP_ID1       0x3D
#define ES7210_REG_CHIP_ID0       0x3E
#define ES7210_REG_CHIP_VERSION   0x3F
#define ES7210_REG_MODE           0x08
#define ES7210_REG_SDP1           0x11
#define ES7210_REG_SDP2           0x12
#define ES7210_REG_MIC12_POWER    0x4B
#define ES7210_REG_MIC1_GAIN      0x43
#define ES7210_REG_MIC2_GAIN      0x44
#define ES7210_CHIP_ID1_EXPECTED  0x72
#define ES7210_CHIP_ID0_EXPECTED  0x10
#define ES7210_PDN_MICBIAS12      (1U << 6)
#define ES7210_MIC_INPUT_ENABLE   (1U << 4)
#define SLOT_ACTIVITY_RATIO       4U
#define SLOT_ACTIVITY_MINIMUM     (MUSIC_CAPTURE_FRAMES / 4U)

typedef struct {
    audio_codec_ctrl_if_t base;
    i2c_master_dev_handle_t device;
    bool open;
} codec_i2c_ctrl_t;

static const char *TAG = "ES7210_CAPTURE";
static i2c_master_bus_handle_t s_i2c_bus;
static codec_i2c_ctrl_t s_ctrl;
static i2s_chan_handle_t s_i2s_rx;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_if_t *s_codec_if;
static esp_codec_dev_handle_t s_codec_dev;
static QueueHandle_t s_free_queue;
static QueueHandle_t s_filled_queue;
static SemaphoreHandle_t s_codec_lock;
static volatile float s_input_gain_db = MUSIC_ES7210_INPUT_GAIN_DB;
static audio_capture_block_t s_blocks[MUSIC_CAPTURE_BUFFER_COUNT];
static int16_t s_interleaved[MUSIC_CAPTURE_FRAMES * BOARD_AUDIO_CHANNELS];

static esp_err_t read_register(uint8_t reg, uint8_t *value);
static esp_err_t write_register(uint8_t reg, uint8_t value);

#if MUSIC_USE_SINGLE_MIC_CH1 && BOARD_AUTO_DETECT_MIC1_SLOT
#define MIC1_SLOT_CONFIRM_BLOCKS          2U

static int s_mic1_slot_index = BOARD_MIC1_SLOT_INDEX;
static int s_mic1_slot_candidate = BOARD_MIC1_SLOT_INDEX;
static unsigned s_mic1_slot_candidate_blocks;
static bool s_mic1_slot_confirmed;

/* Measure AC activity with first differences. This rejects DC offsets and costs
 * only integer subtract/abs/add operations. */
static void measure_slot_activity(uint32_t activity[BOARD_AUDIO_CHANNELS])
{
    int32_t previous[BOARD_AUDIO_CHANNELS];
    for (int slot = 0; slot < BOARD_AUDIO_CHANNELS; ++slot) {
        previous[slot] = s_interleaved[slot];
        activity[slot] = 0;
    }
    for (size_t frame = 1; frame < MUSIC_CAPTURE_FRAMES; ++frame) {
        for (int slot = 0; slot < BOARD_AUDIO_CHANNELS; ++slot) {
            const int32_t sample = s_interleaved[frame * BOARD_AUDIO_CHANNELS + slot];
            const int32_t delta = sample - previous[slot];
            activity[slot] += (uint32_t)(delta < 0 ? -delta : delta);
            previous[slot] = sample;
        }
    }
}

static int detect_mic1_slot(void)
{
    uint32_t activity[BOARD_AUDIO_CHANNELS];
    measure_slot_activity(activity);
    const int strongest = activity[1] > activity[0] ? 1 : 0;
    const int other = 1 - strongest;
    const bool decisive = activity[strongest] >= SLOT_ACTIVITY_MINIMUM &&
                          (uint64_t)activity[strongest] >=
                              (uint64_t)activity[other] * SLOT_ACTIVITY_RATIO;

    if (!decisive) {
        s_mic1_slot_candidate_blocks = 0;
        return s_mic1_slot_index;
    }
    if (s_mic1_slot_candidate != strongest) {
        s_mic1_slot_candidate = strongest;
        s_mic1_slot_candidate_blocks = 1;
    } else if (s_mic1_slot_candidate_blocks < MIC1_SLOT_CONFIRM_BLOCKS) {
        ++s_mic1_slot_candidate_blocks;
    }
    if (s_mic1_slot_candidate_blocks >= MIC1_SLOT_CONFIRM_BLOCKS &&
        (!s_mic1_slot_confirmed || s_mic1_slot_index != strongest)) {
        const bool corrected = s_mic1_slot_index != strongest;
        s_mic1_slot_index = strongest;
        s_mic1_slot_confirmed = true;
        if (corrected) {
            ESP_LOGW(TAG, "CN1/MIC1 I2S mapping corrected to slot%d (activity=[%" PRIu32 ",%" PRIu32 "])",
                     strongest, activity[0], activity[1]);
        } else {
            ESP_LOGI(TAG, "CN1/MIC1 I2S mapping confirmed at slot%d (activity=[%" PRIu32 ",%" PRIu32 "])",
                     strongest, activity[0], activity[1]);
        }
    }
    return s_mic1_slot_index;
}
#endif

#if !MUSIC_USE_SINGLE_MIC_CH1
static esp_err_t validate_dual_inputs(void)
{
    uint8_t mic1_gain = 0;
    uint8_t mic2_gain = 0;
    if (read_register(ES7210_REG_MIC1_GAIN, &mic1_gain) != ESP_OK ||
        read_register(ES7210_REG_MIC2_GAIN, &mic2_gain) != ESP_OK) {
        ESP_LOGE(TAG, "dual input validation failed: gain register read failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "MIC gain readback: MIC1=0x%02X MIC2=0x%02X (gain codes %u/%u)",
             mic1_gain, mic2_gain, mic1_gain & 0x0F, mic2_gain & 0x0F);
    if ((mic1_gain & ES7210_MIC_INPUT_ENABLE) == 0 ||
        (mic2_gain & ES7210_MIC_INPUT_ENABLE) == 0) {
        ESP_LOGE(TAG, "dual input enable readback failed: MIC1=0x%02X MIC2=0x%02X",
                 mic1_gain, mic2_gain);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if ((mic1_gain & 0x0F) != (mic2_gain & 0x0F)) {
        ESP_LOGW(TAG, "MIC gain codes differ; quality selector will compensate without mixing channels");
    }
    ESP_LOGI(TAG,
             "dual input read-only validation passed; fixed mapping MIC1=slot%d MIC2=slot%d",
             BOARD_MIC1_SLOT_INDEX, BOARD_MIC2_SLOT_INDEX);
    return ESP_OK;
}
#endif

static int ctrl_open(const audio_codec_ctrl_if_t *interface, void *configuration, int size)
{
    (void)configuration;
    (void)size;
    codec_i2c_ctrl_t *ctrl = (codec_i2c_ctrl_t *)interface;
    ctrl->open = ctrl->device != NULL;
    return ctrl->open ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRONG_STATE;
}

static bool ctrl_is_open(const audio_codec_ctrl_if_t *interface)
{
    return interface != NULL && ((const codec_i2c_ctrl_t *)interface)->open;
}

static int ctrl_read(const audio_codec_ctrl_if_t *interface, int reg, int reg_len,
                     void *data, int data_len)
{
    if (!ctrl_is_open(interface) || data == NULL || reg_len != 1 || data_len <= 0) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    const uint8_t address = (uint8_t)reg;
    const esp_err_t error = i2c_master_transmit_receive(((const codec_i2c_ctrl_t *)interface)->device,
                                                        &address, 1, data, data_len, 100);
    return error == ESP_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_READ_FAIL;
}

static int ctrl_write(const audio_codec_ctrl_if_t *interface, int reg, int reg_len,
                      void *data, int data_len)
{
    if (!ctrl_is_open(interface) || data == NULL || reg_len != 1 || data_len <= 0 || data_len > 3) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    uint8_t bytes[4] = {(uint8_t)reg, 0, 0, 0};
    memcpy(&bytes[1], data, data_len);
    const esp_err_t error = i2c_master_transmit(((const codec_i2c_ctrl_t *)interface)->device,
                                                bytes, data_len + 1, 100);
    return error == ESP_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRITE_FAIL;
}

static int ctrl_close(const audio_codec_ctrl_if_t *interface)
{
    if (interface == NULL) return ESP_CODEC_DEV_INVALID_ARG;
    ((codec_i2c_ctrl_t *)interface)->open = false;
    return ESP_CODEC_DEV_OK;
}

static esp_err_t read_register(uint8_t reg, uint8_t *value)
{
    return ctrl_read(&s_ctrl.base, reg, 1, value, 1) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t write_register(uint8_t reg, uint8_t value)
{
    return ctrl_write(&s_ctrl.base, reg, 1, &value, 1) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t initialize_i2c(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_i2c_bus), TAG, "I2C bus init failed");
    ESP_LOGI(TAG, "I2C initialized, SDA=%d SCL=%d speed=%dHz", BOARD_I2C_SDA_GPIO,
             BOARD_I2C_SCL_GPIO, BOARD_I2C_SPEED_HZ);
    bool expected_found = false;
    for (uint8_t address = 1; address < 0x7F; ++address) {
        if (i2c_master_probe(s_i2c_bus, address, 10) == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02X", address);
            if (address == BOARD_ES7210_I2C_ADDR_7BIT) expected_found = true;
        }
    }
    if (!expected_found) {
        ESP_LOGE(TAG, "ES7210 did not acknowledge required 7-bit address 0x%02X",
                 BOARD_ES7210_I2C_ADDR_7BIT);
        return ESP_ERR_NOT_FOUND;
    }
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_ES7210_I2C_ADDR_7BIT,
        .scl_speed_hz = BOARD_I2C_SPEED_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &device_config, &s_ctrl.device),
                        TAG, "adding ES7210 I2C device failed");
    s_ctrl.base.open = ctrl_open;
    s_ctrl.base.is_open = ctrl_is_open;
    s_ctrl.base.read_reg = ctrl_read;
    s_ctrl.base.write_reg = ctrl_write;
    s_ctrl.base.close = ctrl_close;
    s_ctrl.open = true;

    uint8_t id1 = 0, id0 = 0, version = 0;
    ESP_RETURN_ON_ERROR(read_register(ES7210_REG_CHIP_ID1, &id1), TAG, "chip ID1 read failed");
    ESP_RETURN_ON_ERROR(read_register(ES7210_REG_CHIP_ID0, &id0), TAG, "chip ID0 read failed");
    ESP_RETURN_ON_ERROR(read_register(ES7210_REG_CHIP_VERSION, &version), TAG, "version read failed");
    ESP_LOGI(TAG, "ES7210 ID=0x%02X%02X version=0x%02X", id1, id0, version);
    if (id1 != ES7210_CHIP_ID1_EXPECTED || id0 != ES7210_CHIP_ID0_EXPECTED) {
        ESP_LOGE(TAG, "unexpected ES7210 ID; audio tasks will not start");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t initialize_i2s(void)
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_PORT, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = 6;
    channel_config.dma_frame_num = 256;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, NULL, &s_i2s_rx), TAG,
                        "I2S RX channel allocation failed");
    i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK_GPIO,
            .bclk = BOARD_I2S_BCLK_GPIO,
            .ws = BOARD_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = BOARD_I2S_DIN_GPIO,
            .invert_flags = {0},
        },
    };
    standard_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_rx, &standard_config), TAG,
                        "I2S standard mode init failed");
    /* esp_codec_dev_open reconfigures the disabled channel, then enables it. */
    return ESP_OK;
}

static esp_err_t initialize_codec(void)
{
    const audio_codec_i2s_cfg_t data_config = {
        .port = BOARD_I2S_PORT,
        .rx_handle = s_i2s_rx,
        .tx_handle = NULL,
    };
    s_data_if = audio_codec_new_i2s_data((audio_codec_i2s_cfg_t *)&data_config);
    ESP_RETURN_ON_FALSE(s_data_if != NULL, ESP_ERR_NO_MEM, TAG, "codec I2S interface allocation failed");

    es7210_codec_cfg_t codec_config = {
        .ctrl_if = &s_ctrl.base,
        .master_mode = false,
#if MUSIC_USE_SINGLE_MIC_CH1
        .mic_selected = ES7210_SEL_MIC1,
#else
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
#endif
        .mclk_src = ES7210_MCLK_FROM_PAD,
        .mclk_div = BOARD_AUDIO_MCLK_MULTIPLE,
    };
    s_codec_if = es7210_codec_new(&codec_config);
    ESP_RETURN_ON_FALSE(s_codec_if != NULL, ESP_FAIL, TAG, "ES7210 codec interface creation failed");
    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_codec_if,
        .data_if = s_data_if,
    };
    s_codec_dev = esp_codec_dev_new(&device_config);
    ESP_RETURN_ON_FALSE(s_codec_dev != NULL, ESP_ERR_NO_MEM, TAG, "codec device creation failed");
    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = BOARD_AUDIO_EFFECTIVE_BITS,
        .channel = BOARD_AUDIO_CHANNELS,
        .channel_mask = 0,
        .sample_rate = BOARD_AUDIO_SAMPLE_RATE_HZ,
        .mclk_multiple = BOARD_AUDIO_MCLK_MULTIPLE,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec_dev, &sample_info) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "opening ES7210 input failed");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(s_codec_dev, MUSIC_ES7210_INPUT_GAIN_DB) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "setting ES7210 input gain failed");
    s_input_gain_db = MUSIC_ES7210_INPUT_GAIN_DB;

    uint8_t power = 0;
    ESP_RETURN_ON_ERROR(read_register(ES7210_REG_MIC12_POWER, &power), TAG, "MIC power read failed");
    power |= ES7210_PDN_MICBIAS12;
    ESP_RETURN_ON_ERROR(write_register(ES7210_REG_MIC12_POWER, power), TAG, "MICBIAS12 disable failed");
    ESP_RETURN_ON_ERROR(read_register(ES7210_REG_MIC12_POWER, &power), TAG, "MIC power verify failed");
    ESP_RETURN_ON_FALSE((power & ES7210_PDN_MICBIAS12) != 0, ESP_ERR_INVALID_RESPONSE, TAG,
                        "MICBIAS12 remained enabled");

    uint8_t mode = 0, sdp1 = 0, sdp2 = 0;
    ESP_RETURN_ON_ERROR(read_register(ES7210_REG_MODE, &mode), TAG, "mode readback failed");
    ESP_RETURN_ON_ERROR(read_register(ES7210_REG_SDP1, &sdp1), TAG, "SDP1 readback failed");
    ESP_RETURN_ON_ERROR(read_register(ES7210_REG_SDP2, &sdp2), TAG, "SDP2 readback failed");
    ESP_RETURN_ON_FALSE((mode & 0x01) == 0 && (sdp1 & 0x63) == 0x60 && sdp2 == 0x00,
                        ESP_ERR_INVALID_RESPONSE, TAG,
                        "ES7210 configuration readback invalid: mode=0x%02X sdp1=0x%02X sdp2=0x%02X",
                        mode, sdp1, sdp2);
#if MUSIC_USE_SINGLE_MIC_CH1
    ESP_LOGI(TAG, "MIC1 enabled, MIC2 disabled/ignored, gain=%.1fdB, MICBIAS12 powered down",
             MUSIC_ES7210_INPUT_GAIN_DB);
#else
    ESP_LOGI(TAG, "MIC1 and MIC2 enabled, gain=%.1fdB, MICBIAS12 powered down",
             MUSIC_ES7210_INPUT_GAIN_DB);
#endif
    ESP_LOGI(TAG, "output=standard I2S slots=%d bits/slot=%d effective_bits=%d MIC1=slot%d MIC2=slot%d",
             BOARD_AUDIO_SLOT_COUNT, BOARD_AUDIO_BITS_PER_SLOT, BOARD_AUDIO_EFFECTIVE_BITS,
             BOARD_MIC1_SLOT_INDEX, BOARD_MIC2_SLOT_INDEX);
#if MUSIC_USE_SINGLE_MIC_CH1 && BOARD_AUTO_DETECT_MIC1_SLOT
    ESP_LOGI(TAG, "CN1/MIC1 slot verification enabled (nominal slot%d, DSP remains single-channel)",
             BOARD_MIC1_SLOT_INDEX);
#endif
    ESP_LOGI(TAG, "sample_rate=%d MCLK=%dHz(GPIO%d) BCLK=%dHz(GPIO%d) LRCK=%dHz(GPIO%d) DIN=GPIO%d",
             BOARD_AUDIO_SAMPLE_RATE_HZ, BOARD_AUDIO_SAMPLE_RATE_HZ * BOARD_AUDIO_MCLK_MULTIPLE,
             BOARD_I2S_MCLK_GPIO, BOARD_AUDIO_SAMPLE_RATE_HZ * BOARD_AUDIO_BITS_PER_SLOT * BOARD_AUDIO_SLOT_COUNT,
             BOARD_I2S_BCLK_GPIO, BOARD_AUDIO_SAMPLE_RATE_HZ, BOARD_I2S_WS_GPIO, BOARD_I2S_DIN_GPIO);
    return ESP_OK;
}

esp_err_t es7210_capture_init(void)
{
    gpio_config_t interrupt_config = {
        .pin_bit_mask = 1ULL << BOARD_ES7210_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&interrupt_config), TAG, "INT input config failed");
    ESP_RETURN_ON_ERROR(initialize_i2c(), TAG, "I2C/identity validation failed");
    ESP_RETURN_ON_ERROR(initialize_i2s(), TAG, "I2S initialization failed");
    ESP_RETURN_ON_ERROR(initialize_codec(), TAG, "codec initialization failed");
#if !MUSIC_USE_SINGLE_MIC_CH1
    ESP_RETURN_ON_ERROR(validate_dual_inputs(), TAG,
                        "dual microphone read-only validation failed");
#endif
    s_codec_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_codec_lock != NULL, ESP_ERR_NO_MEM, TAG,
                        "codec mutex allocation failed");
    s_free_queue = xQueueCreate(MUSIC_CAPTURE_BUFFER_COUNT, sizeof(int));
    s_filled_queue = xQueueCreate(MUSIC_CAPTURE_BUFFER_COUNT, sizeof(int));
    ESP_RETURN_ON_FALSE(s_free_queue && s_filled_queue, ESP_ERR_NO_MEM, TAG, "capture queue allocation failed");
    for (int index = 0; index < MUSIC_CAPTURE_BUFFER_COUNT; ++index) {
        xQueueSend(s_free_queue, &index, 0);
    }
    return ESP_OK;
}

static void audio_capture_task(void *argument)
{
    (void)argument;
    diagnostics_counters_t *counters = diagnostics_counters();
    uint32_t last_stack_check_ms = 0;
    bool low_stack_warned = false;
    while (true) {
        int index = -1;
        if (xQueueReceive(s_free_queue, &index, pdMS_TO_TICKS(100)) != pdTRUE) {
            ++counters->buffer_exhaustion_count;
            ++counters->dropped_buffer_count;
            continue;
        }
        xSemaphoreTake(s_codec_lock, portMAX_DELAY);
        const int error = esp_codec_dev_read(s_codec_dev, s_interleaved,
                                             sizeof(s_interleaved));
        xSemaphoreGive(s_codec_lock);
        if (error != ESP_CODEC_DEV_OK) {
            ++counters->i2s_read_error_count;
            xQueueSend(s_free_queue, &index, portMAX_DELAY);
            continue;
        }
        audio_capture_block_t *block = &s_blocks[index];
#if MUSIC_USE_SINGLE_MIC_CH1 && BOARD_AUTO_DETECT_MIC1_SLOT
        const int mic1_slot_index = detect_mic1_slot();
#else
        const int mic1_slot_index = BOARD_MIC1_SLOT_INDEX;
#endif
        for (size_t frame = 0; frame < MUSIC_CAPTURE_FRAMES; ++frame) {
            block->mic1[frame] = s_interleaved[frame * BOARD_AUDIO_CHANNELS + mic1_slot_index];
#if !MUSIC_USE_SINGLE_MIC_CH1
            block->mic2[frame] = s_interleaved[frame * BOARD_AUDIO_CHANNELS + BOARD_MIC2_SLOT_INDEX];
#endif
        }
        block->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (block->timestamp_ms - last_stack_check_ms >=
            MUSIC_CAPTURE_STACK_CHECK_INTERVAL_MS) {
            last_stack_check_ms = block->timestamp_ms;
            const UBaseType_t stack_bytes = uxTaskGetStackHighWaterMark(NULL);
            if (!low_stack_warned &&
                stack_bytes < MUSIC_CAPTURE_STACK_WARN_BYTES) {
                low_stack_warned = true;
                ESP_LOGW(TAG, "AudioCaptureTask low stack: %u bytes remaining",
                         (unsigned)stack_bytes);
            }
        }
        if (xQueueSend(s_filled_queue, &index, 0) != pdTRUE) {
            ++counters->queue_overflow_count;
            ++counters->dropped_buffer_count;
            xQueueSend(s_free_queue, &index, portMAX_DELAY);
        }
    }
}

esp_err_t es7210_capture_start(void)
{
    const BaseType_t result = xTaskCreatePinnedToCore(
        audio_capture_task, "AudioCaptureTask", MUSIC_CAPTURE_TASK_STACK_SIZE,
        NULL, 22, NULL, 0);
    if (result == pdPASS) {
        ESP_LOGI(TAG, "AudioCaptureTask started: stack=%u core=0 priority=22",
                 (unsigned)MUSIC_CAPTURE_TASK_STACK_SIZE);
    }
    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool supported_gain(float gain_db)
{
    static const float gains[] = {
        MUSIC_ES7210_GAIN_OPTION_LOW_1_DB,
        MUSIC_ES7210_GAIN_OPTION_LOW_2_DB,
        MUSIC_ES7210_GAIN_OPTION_1_DB,
        MUSIC_ES7210_GAIN_OPTION_2_DB,
        MUSIC_ES7210_GAIN_OPTION_3_DB,
        MUSIC_ES7210_GAIN_OPTION_4_DB,
        MUSIC_ES7210_GAIN_OPTION_5_DB,
        MUSIC_ES7210_GAIN_OPTION_6_DB,
        MUSIC_ES7210_GAIN_OPTION_7_DB,
        MUSIC_ES7210_GAIN_OPTION_8_DB,
    };
    for (size_t index = 0; index < sizeof(gains) / sizeof(gains[0]); ++index) {
        if (fabsf(gain_db - gains[index]) < 0.1f) return true;
    }
    return false;
}

esp_err_t es7210_capture_set_input_gain(float gain_db)
{
    if (!supported_gain(gain_db)) return ESP_ERR_INVALID_ARG;
    if (s_codec_dev == NULL || s_codec_lock == NULL) return ESP_ERR_INVALID_STATE;
    if (fabsf(s_input_gain_db - gain_db) < 0.1f) return ESP_OK;
    if (xSemaphoreTake(s_codec_lock, pdMS_TO_TICKS(250)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const int result = esp_codec_dev_set_in_gain(s_codec_dev, gain_db);
    if (result == ESP_CODEC_DEV_OK) s_input_gain_db = gain_db;
    xSemaphoreGive(s_codec_lock);
    return result == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

float es7210_capture_get_input_gain(void)
{
    return s_input_gain_db;
}

int es7210_capture_take_block(audio_capture_block_t **block, uint32_t timeout_ms)
{
    int index = -1;
    if (xQueueReceive(s_filled_queue, &index, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return -1;
    *block = &s_blocks[index];
    return index;
}

void es7210_capture_release_block(int index)
{
    if (index >= 0 && index < MUSIC_CAPTURE_BUFFER_COUNT) xQueueSend(s_free_queue, &index, portMAX_DELAY);
}

unsigned es7210_capture_queue_depth(void)
{
    return s_filled_queue ? uxQueueMessagesWaiting(s_filled_queue) : 0;
}
