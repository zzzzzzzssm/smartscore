#pragma once

#include <cstdint>

namespace dl {
class Model;
namespace image {
class ImagePreprocessor;
}
} // namespace dl

struct PointDirectionResult {
    bool valid = false;
    int class_index = -1;
    const char *label = "other";
    float confidence = 0.0F;
    float margin = 0.0F;
};

class PointDirectionAi {
public:
    ~PointDirectionAi();

    bool init();
    PointDirectionResult infer(uint8_t *rgb888,
                               uint16_t width,
                               uint16_t height,
                               int box_x1,
                               int box_y1,
                               int box_x2,
                               int box_y2);

private:
    void release();

    dl::Model *model_ = nullptr;
    dl::image::ImagePreprocessor *preprocessor_ = nullptr;
};
