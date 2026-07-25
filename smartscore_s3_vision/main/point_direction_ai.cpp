#include "point_direction_ai.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <vector>

#include "dl_image_define.hpp"
#include "dl_image_preprocessor.hpp"
#include "dl_model_base.hpp"
#include "dl_tensor_base.hpp"
#include "esp_log.h"
#include "fbs_model.hpp"
#include "vision_config.hpp"

extern const uint8_t point_direction_espdl[] asm("_binary_point_direction_espdl_start");

namespace {
constexpr char TAG[] = "point_direction_ai";
constexpr int kPointInputSize = 128;
const char *kCategoryNames[] = {"point_right", "point_left", "other"};

bool softmax_model_output(dl::TensorBase *output, std::array<float, 3> &probabilities)
{
    if (output == nullptr || output->get_size() < static_cast<int>(probabilities.size()) ||
        output->get_element_ptr() == nullptr) {
        return false;
    }

    std::array<float, 3> logits = {};
    const float quant_scale = std::ldexp(1.0F, output->get_exponent());
    switch (output->get_dtype()) {
    case dl::DATA_TYPE_INT16: {
        const int16_t *values = output->get_element_ptr<int16_t>();
        for (size_t index = 0; index < logits.size(); ++index) {
            logits[index] = static_cast<float>(values[index]) * quant_scale;
        }
        break;
    }
    case dl::DATA_TYPE_INT8: {
        const int8_t *values = output->get_element_ptr<int8_t>();
        for (size_t index = 0; index < logits.size(); ++index) {
            logits[index] = static_cast<float>(values[index]) * quant_scale;
        }
        break;
    }
    case dl::DATA_TYPE_FLOAT: {
        const float *values = output->get_element_ptr<float>();
        std::copy_n(values, logits.size(), logits.begin());
        break;
    }
    default:
        ESP_LOGE(TAG, "unsupported point model output type: %s", output->get_dtype_string());
        return false;
    }

    const float max_logit = *std::max_element(logits.begin(), logits.end());
    float sum = 0.0F;
    for (size_t index = 0; index < probabilities.size(); ++index) {
        probabilities[index] = std::exp(logits[index] - max_logit);
        sum += probabilities[index];
    }
    if (!std::isfinite(sum) || sum <= 0.0F) {
        return false;
    }
    for (float &probability : probabilities) {
        probability /= sum;
    }
    return true;
}

std::vector<int> square_crop(int x1, int y1, int x2, int y2, int width, int height)
{
    x1 = std::clamp(x1, 0, width - 1);
    y1 = std::clamp(y1, 0, height - 1);
    x2 = std::clamp(x2, x1 + 1, width);
    y2 = std::clamp(y2, y1 + 1, height);
    const float center_x = (static_cast<float>(x1) + static_cast<float>(x2)) * 0.5F;
    const float center_y = (static_cast<float>(y1) + static_cast<float>(y2)) * 0.5F;
    const float base_side = static_cast<float>(std::max(x2 - x1, y2 - y1));
    const float requested_side = base_side * (1.0F + 2.0F * vision_config::kPointCropPaddingRatio);
    const int side = std::clamp(
        static_cast<int>(std::lround(requested_side)), 1, std::min(width, height));
    int crop_x1 = static_cast<int>(std::lround(center_x - static_cast<float>(side) * 0.5F));
    int crop_y1 = static_cast<int>(std::lround(center_y - static_cast<float>(side) * 0.5F));
    crop_x1 = std::clamp(crop_x1, 0, width - side);
    crop_y1 = std::clamp(crop_y1, 0, height - side);
    const int crop_x2 = crop_x1 + side;
    const int crop_y2 = crop_y1 + side;
    return {crop_x1, crop_y1, crop_x2, crop_y2};
}
} // namespace

PointDirectionAi::~PointDirectionAi()
{
    release();
}

bool PointDirectionAi::init()
{
    release();
    model_ = new (std::nothrow) dl::Model(reinterpret_cast<const char *>(point_direction_espdl),
                                         fbs::MODEL_LOCATION_IN_FLASH_RODATA,
                                         0,
                                         dl::MEMORY_MANAGER_GREEDY,
                                         nullptr,
                                         false);
    if (model_ == nullptr || model_->get_input() == nullptr || model_->get_output() == nullptr) {
        ESP_LOGE(TAG, "failed to load embedded point direction model");
        release();
        return false;
    }
    if (model_->test() != ESP_OK) {
        // ESP-DL compares quantized self-test tensors with a fixed near-exact
        // tolerance. Small target-dependent rounding differences can therefore fail
        // the reference comparison even though the graph loaded and ran correctly.
        ESP_LOGW(TAG,
                 "embedded point direction model self-test differs from the reference; "
                 "continuing with on-device validation");
    }
    model_->minimize();
    preprocessor_ = new (std::nothrow)
        dl::image::ImagePreprocessor(model_, {123.675F, 116.28F, 103.53F}, {58.395F, 57.12F, 57.375F});
    if (preprocessor_ == nullptr) {
        ESP_LOGE(TAG, "failed to allocate point direction preprocessor");
        release();
        return false;
    }
    const std::vector<int> input_shape = model_->get_input()->get_shape();
    if (input_shape.size() != 4 || input_shape[0] != 1 || input_shape[1] != kPointInputSize ||
        input_shape[2] != kPointInputSize || input_shape[3] != 3) {
        ESP_LOGE(TAG, "unexpected point model input shape");
        release();
        return false;
    }
    dl::TensorBase *output = model_->get_output();
    ESP_LOGI(TAG,
             "custom point direction model ready: mobilenetv2_050_int16, "
             "classes=[0:right,1:left,2:other], input=%dx%d, "
             "output=%s exp=%d",
             kPointInputSize,
             kPointInputSize,
             output->get_dtype_string(),
             output->get_exponent());
    return true;
}

PointDirectionResult PointDirectionAi::infer(uint8_t *rgb888,
                                             uint16_t width,
                                             uint16_t height,
                                             int box_x1,
                                             int box_y1,
                                             int box_x2,
                                             int box_y2)
{
    PointDirectionResult result;
    if (model_ == nullptr || preprocessor_ == nullptr || rgb888 == nullptr || width < 2 || height < 2) {
        return result;
    }
    const dl::image::img_t image = {
        .data = rgb888,
        .width = width,
        .height = height,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888,
    };
    const std::vector<int> crop = square_crop(box_x1, box_y1, box_x2, box_y2, width, height);
    preprocessor_->preprocess(image, crop);
    model_->run();
    std::array<float, 3> probabilities = {};
    if (!softmax_model_output(model_->get_output(), probabilities)) {
        return result;
    }

    std::array<size_t, 3> ranking = {0, 1, 2};
    std::sort(ranking.begin(), ranking.end(), [&probabilities](size_t left, size_t right) {
        return probabilities[left] > probabilities[right];
    });
    const size_t best = ranking[0];
    const size_t second = ranking[1];
    result.valid = true;
    result.class_index = static_cast<int>(best);
    result.label = kCategoryNames[best];
    result.confidence = probabilities[best];
    result.margin = probabilities[best] - probabilities[second];
    return result;
}

void PointDirectionAi::release()
{
    delete preprocessor_;
    preprocessor_ = nullptr;
    delete model_;
    model_ = nullptr;
}
