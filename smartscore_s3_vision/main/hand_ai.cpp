#include "hand_ai.hpp"

#include <algorithm>
#include <cstring>
#include <list>
#include <new>
#include <vector>

#include "dl_image_define.hpp"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "hand_detect.hpp"
#include "hand_gesture_category_name.hpp"
#include "hand_gesture_recognition.hpp"
#include "official_gesture_guard.hpp"
#include "point_direction_ai.hpp"
#include "vision_config.hpp"

namespace {
constexpr char TAG[] = "hand_ai";
constexpr size_t kCategoryCount = sizeof(hand_gesture_cat_names) / sizeof(hand_gesture_cat_names[0]);

constexpr uint32_t kPointModelGuardMagic = 0x50444D47U;
constexpr uint32_t kPointModelInitIdle = 0;
constexpr uint32_t kPointModelInitInProgress = 1;
constexpr uint32_t kPointModelInitQuarantined = 2;

struct PointModelBootGuard {
    uint32_t magic;
    uint32_t firmware_signature;
    uint32_t state;
};

RTC_NOINIT_ATTR PointModelBootGuard s_point_model_guard;

uint32_t firmware_signature()
{
    const esp_app_desc_t *description = esp_app_get_description();
    uint32_t signature = 0;
    if (description != nullptr) {
        std::memcpy(&signature, description->app_elf_sha256, sizeof(signature));
    }
    return signature;
}

bool point_model_is_quarantined()
{
    const uint32_t signature = firmware_signature();
    if (s_point_model_guard.magic != kPointModelGuardMagic ||
        s_point_model_guard.firmware_signature != signature) {
        s_point_model_guard.magic = kPointModelGuardMagic;
        s_point_model_guard.firmware_signature = signature;
        s_point_model_guard.state = kPointModelInitIdle;
        return false;
    }
    if (s_point_model_guard.state == kPointModelInitInProgress) {
        s_point_model_guard.state = kPointModelInitQuarantined;
        ESP_LOGE(TAG,
                 "previous custom point model initialization reset the CPU; actions 1 and 2 are quarantined");
    }
    return s_point_model_guard.state == kPointModelInitQuarantined;
}

void point_model_init_started()
{
    s_point_model_guard.state = kPointModelInitInProgress;
    __sync_synchronize();
}

void point_model_init_finished(bool success)
{
    s_point_model_guard.state = success ? kPointModelInitIdle : kPointModelInitQuarantined;
    __sync_synchronize();
}

bool is_official_control_gesture(StaticGesture gesture)
{
    return gesture == StaticGesture::OK || gesture == StaticGesture::OPEN_PALM ||
           gesture == StaticGesture::THUMB_UP;
}

class TopKHandGestureCls final : public HandGestureCls {
public:
    TopKHandGestureCls() : HandGestureCls(HandGestureCls::MOBILENETV2_0_5_S8_V1, true)
    {
        // hand_gesture_recognition 0.1.2 defaults to Top-1 and its public
        // set_topk() cannot update a loaded wrapper. Initialize the protected
        // wrapper settings before its existing lazy-load path creates the
        // official MobileNetV2 implementation.
        m_model = nullptr;
        m_topk = vision_config::kOfficialGestureTopK;
    }
};
} // namespace

HandAi::~HandAi()
{
    delete point_direction_;
    delete classifier_;
    delete detector_;
}

bool HandAi::init()
{
    ESP_LOGI(TAG, "official hand gesture categories (%u):", static_cast<unsigned>(kCategoryCount));
    for (size_t index = 0; index < kCategoryCount; ++index) {
        ESP_LOGI(TAG, "  [%u] %s", static_cast<unsigned>(index), hand_gesture_cat_names[index]);
    }

    ok_supported_ = category_supported("ok");
    open_palm_supported_ = category_supported("five");
    thumb_up_supported_ = category_supported("like");
    if (!ok_supported_) {
        ESP_LOGW(TAG, "model category 'ok' missing; action 3 disabled");
    }
    if (!open_palm_supported_) {
        ESP_LOGW(TAG, "model category 'five' missing; action 4 disabled");
    }
    if (!thumb_up_supported_) {
        ESP_LOGW(TAG, "model category 'like' missing; action 5 disabled");
    }

    detector_ = new (std::nothrow) HandDetect(HandDetect::ESPDET_PICO_224_224_HAND, false);
    classifier_ = new (std::nothrow) TopKHandGestureCls();
    if (detector_ == nullptr || classifier_ == nullptr) {
        ESP_LOGE(TAG, "failed to allocate official hand AI wrappers");
        return false;
    }
    if (point_model_is_quarantined()) {
        ESP_LOGW(TAG,
                 "custom point direction model skipped for this firmware; actions 1 and 2 disabled, "
                 "actions 3 to 5 remain available");
    } else {
        point_model_init_started();
        point_direction_ = new (std::nothrow) PointDirectionAi();
        const bool point_model_ready = point_direction_ != nullptr && point_direction_->init();
        point_model_init_finished(point_model_ready);
        if (!point_model_ready) {
            ESP_LOGW(TAG,
                     "custom point direction model unavailable; actions 1 and 2 disabled, "
                     "actions 3 to 5 remain available");
            delete point_direction_;
            point_direction_ = nullptr;
        }
    }
    ESP_LOGI(TAG,
             "official hand AI ready; official topk=%d priority=%.0f%% veto>=%.0f%%; "
             "custom point=MobileNetV2-0.5 INT16 hand>=%.0f%% "
             "score>=%.0f%% margin>=%.0f%% one-shot entry_grace=1 frame",
             vision_config::kOfficialGestureTopK,
             static_cast<double>(vision_config::kOfficialGesturePriorityConfidence * 100.0F),
             static_cast<double>(vision_config::kOfficialGestureVetoConfidence * 100.0F),
             static_cast<double>(vision_config::kPointDirectionHandConfidence * 100.0F),
             static_cast<double>(vision_config::kPointDirectionConfidence * 100.0F),
             static_cast<double>(vision_config::kPointDirectionMargin * 100.0F));
    return true;
}

HandObservation HandAi::infer(uint8_t *rgb888, uint16_t width, uint16_t height)
{
    HandObservation observation;
    if (detector_ == nullptr || classifier_ == nullptr || rgb888 == nullptr) {
        return observation;
    }

    dl::image::img_t image = {
        .data = rgb888,
        .width = width,
        .height = height,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888,
    };
    std::list<dl::detect::result_t> &detections = detector_->run(image);

    const dl::detect::result_t *best = nullptr;
    for (const auto &detection : detections) {
        if (detection.box.size() < 4 || detection.score < vision_config::kHandDetectConfidence) {
            continue;
        }
        ++observation.hand_count;
        if (best == nullptr || detection.score > best->score) {
            best = &detection;
        }
    }
    if (best == nullptr) {
        entry_grace_.reset();
        return observation;
    }

    observation.detected = true;
    observation.box_x1 = std::clamp(best->box[0], 0, static_cast<int>(width) - 1);
    observation.box_y1 = std::clamp(best->box[1], 0, static_cast<int>(height) - 1);
    observation.box_x2 = std::clamp(best->box[2], observation.box_x1 + 1, static_cast<int>(width));
    observation.box_y2 = std::clamp(best->box[3], observation.box_y1 + 1, static_cast<int>(height));
    observation.center_x = (observation.box_x1 + observation.box_x2) / 2;
    observation.center_y = (observation.box_y1 + observation.box_y2) / 2;
    observation.detection_confidence = best->score;
    observation.in_control_zone =
        observation.center_y <= static_cast<int>(height * vision_config::kControlZoneBottomRatio);
    if (!observation.in_control_zone) {
        entry_grace_.reset();
        return observation;
    }

    const std::vector<int> crop_area = {
        observation.box_x1, observation.box_y1, observation.box_x2, observation.box_y2};
    const std::vector<dl::cls::result_t> results = classifier_->run_crop(image, crop_area);
    observation.point_horizontal_ratio =
        static_cast<float>(observation.center_x) / static_cast<float>(width);
    if (!results.empty() && results.front().cat_name != nullptr) {
        observation.gesture_label = results.front().cat_name;
        observation.gesture = map_label(results.front().cat_name);
        observation.gesture_confidence = results.front().score;
    }

    const bool official_entry_grace_frame = entry_grace_.consume();

    const bool official_control_gesture =
        is_official_control_gesture(observation.gesture) &&
        observation.gesture_confidence >= vision_config::kOfficialGesturePriorityConfidence;
    if (official_control_gesture) {
        observation.point_zone = "official";
        return observation;
    }

    const dl::cls::result_t *protected_candidate =
        official_gesture_guard::find_protected_candidate(
            results, vision_config::kOfficialGestureVetoConfidence);
    if (protected_candidate != nullptr) {
        observation.gesture = StaticGesture::OTHER;
        observation.gesture_label = official_gesture_guard::veto_label(protected_candidate->cat_name);
        observation.gesture_confidence = protected_candidate->score;
        observation.point_zone = "official_veto";
        observation.official_gesture_veto = true;
        return observation;
    }

    if (official_entry_grace_frame) {
        observation.gesture = StaticGesture::OTHER;
        observation.point_zone = "official_entry_grace";
        return observation;
    }

    if (point_direction_ == nullptr) {
        observation.point_zone = "unavailable";
        return observation;
    }
    if (observation.hand_count != 1) {
        observation.point_zone = "multi_hand";
        observation.gesture = StaticGesture::OTHER;
        observation.gesture_label = "point_multi_hand";
        return observation;
    }
    if (observation.detection_confidence < vision_config::kPointDirectionHandConfidence) {
        observation.point_zone = "weak_hand";
        observation.gesture = StaticGesture::OTHER;
        observation.gesture_label = "point_weak_hand";
        observation.gesture_confidence = 0.0F;
        return observation;
    }

    observation.point_zone = "custom";
    const PointDirectionResult direction = point_direction_->infer(rgb888,
                                                                    width,
                                                                    height,
                                                                    observation.box_x1,
                                                                    observation.box_y1,
                                                                    observation.box_x2,
                                                                    observation.box_y2);
    observation.gesture_label = direction.label;
    observation.gesture_confidence = direction.confidence;
    observation.point_margin = direction.margin;
    StaticGesture point_candidate = StaticGesture::OTHER;
    if (direction.valid && direction.class_index == 0) {
        point_candidate = StaticGesture::POINT_RIGHT;
    } else if (direction.valid && direction.class_index == 1) {
        point_candidate = StaticGesture::POINT_LEFT;
    }

    const bool point_result = point_candidate != StaticGesture::OTHER &&
                              direction.confidence >= vision_config::kPointDirectionConfidence &&
                              direction.margin >= vision_config::kPointDirectionMargin;
    observation.gesture = point_result ? point_candidate : StaticGesture::OTHER;
    return observation;
}

StaticGesture HandAi::map_label(const char *label) const
{
    if (label == nullptr) {
        return StaticGesture::UNKNOWN;
    }
    if (ok_supported_ && std::strcmp(label, "ok") == 0) {
        return StaticGesture::OK;
    }
    if (open_palm_supported_ && std::strcmp(label, "five") == 0) {
        return StaticGesture::OPEN_PALM;
    }
    if (thumb_up_supported_ && std::strcmp(label, "like") == 0) {
        return StaticGesture::THUMB_UP;
    }
    return StaticGesture::OTHER;
}

bool HandAi::category_supported(const char *label) const
{
    for (size_t index = 0; index < kCategoryCount; ++index) {
        if (std::strcmp(hand_gesture_cat_names[index], label) == 0) {
            return true;
        }
    }
    return false;
}
