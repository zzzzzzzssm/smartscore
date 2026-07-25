#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_camera.h"
#include "esp_err.h"
#include "vision_config.hpp"

class CameraDriver {
public:
    esp_err_t init();
    camera_fb_t *acquire_frame();
    void return_frame(camera_fb_t *&frame);
    bool decode_rgb888(const camera_fb_t &frame, uint8_t *rgb_buffer, size_t rgb_buffer_size) const;

    static constexpr size_t rgb_buffer_size();

private:
    bool initialized_ = false;
};

constexpr size_t CameraDriver::rgb_buffer_size()
{
    return static_cast<size_t>(vision_config::kFrameWidth) * vision_config::kFrameHeight * 3U;
}
