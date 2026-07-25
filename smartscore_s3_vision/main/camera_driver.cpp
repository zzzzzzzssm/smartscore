#include "camera_driver.hpp"

#include "board_pins.hpp"
#include "esp_log.h"
#include "img_converters.h"
#include "vision_config.hpp"

namespace {
constexpr char TAG[] = "camera";
}

esp_err_t CameraDriver::init()
{
    camera_config_t config = {};
    config.pin_pwdn = board::CAM_PIN_PWDN;
    config.pin_reset = board::CAM_PIN_RESET;
    config.pin_xclk = board::CAM_PIN_XCLK;
    config.pin_sccb_sda = board::CAM_PIN_SIOD;
    config.pin_sccb_scl = board::CAM_PIN_SIOC;
    config.pin_d0 = board::CAM_PIN_D0;
    config.pin_d1 = board::CAM_PIN_D1;
    config.pin_d2 = board::CAM_PIN_D2;
    config.pin_d3 = board::CAM_PIN_D3;
    config.pin_d4 = board::CAM_PIN_D4;
    config.pin_d5 = board::CAM_PIN_D5;
    config.pin_d6 = board::CAM_PIN_D6;
    config.pin_d7 = board::CAM_PIN_D7;
    config.pin_vsync = board::CAM_PIN_VSYNC;
    config.pin_href = board::CAM_PIN_HREF;
    config.pin_pclk = board::CAM_PIN_PCLK;
    config.xclk_freq_hz = vision_config::kCameraXclkHz;
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = vision_config::kUseQvga ? FRAMESIZE_QVGA : FRAMESIZE_VGA;
    config.jpeg_quality = vision_config::kJpegQuality;
    config.fb_count = vision_config::kCameraFrameBufferCount;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.sccb_i2c_port = 0;

    const esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OV3660 initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == nullptr) {
        ESP_LOGE(TAG, "camera initialized without a sensor handle");
        esp_camera_deinit();
        return ESP_ERR_NOT_FOUND;
    }
    if (sensor->id.PID != OV3660_PID) {
        ESP_LOGW(TAG, "expected OV3660, detected PID=0x%04x", sensor->id.PID);
    }
    if (sensor->set_vflip(sensor, 1) != 0) {
        ESP_LOGE(TAG, "failed to enable camera vertical flip");
        esp_camera_deinit();
        return ESP_FAIL;
    }

    initialized_ = true;
    ESP_LOGI(TAG,
             "camera ready: %ux%u JPEG quality=%d fb_count=%u vflip=on",
             vision_config::kFrameWidth,
             vision_config::kFrameHeight,
             vision_config::kJpegQuality,
             static_cast<unsigned>(vision_config::kCameraFrameBufferCount));
    return ESP_OK;
}

camera_fb_t *CameraDriver::acquire_frame()
{
    return initialized_ ? esp_camera_fb_get() : nullptr;
}

void CameraDriver::return_frame(camera_fb_t *&frame)
{
    if (frame != nullptr) {
        esp_camera_fb_return(frame);
        frame = nullptr;
    }
}

bool CameraDriver::decode_rgb888(const camera_fb_t &frame, uint8_t *rgb_buffer, size_t rgb_buffer_size) const
{
    if (frame.buf == nullptr || frame.format != PIXFORMAT_JPEG || rgb_buffer == nullptr) {
        return false;
    }
    const size_t required = frame.width * frame.height * 3U;
    if (required > rgb_buffer_size) {
        ESP_LOGE(TAG,
                 "RGB buffer too small: need %u, have %u",
                 static_cast<unsigned>(required),
                 static_cast<unsigned>(rgb_buffer_size));
        return false;
    }
    return fmt2rgb888(frame.buf, frame.len, frame.format, rgb_buffer);
}
