#pragma once

#include <cstddef>
#include <cstdint>

#include "official_gesture_entry_grace.hpp"

class HandDetect;
class HandGestureCls;
class PointDirectionAi;

enum class StaticGesture : uint8_t {
    UNKNOWN = 0,
    OK,
    OPEN_PALM,
    THUMB_UP,
    POINT_RIGHT,
    POINT_LEFT,
    OTHER,
};

struct HandObservation {
    bool detected = false;
    uint8_t hand_count = 0;
    bool in_control_zone = false;
    int box_x1 = 0;
    int box_y1 = 0;
    int box_x2 = 0;
    int box_y2 = 0;
    int center_x = 0;
    int center_y = 0;
    float detection_confidence = 0.0F;
    StaticGesture gesture = StaticGesture::UNKNOWN;
    float gesture_confidence = 0.0F;
    const char *gesture_label = "not_classified";
    float point_horizontal_ratio = 0.0F;
    const char *point_zone = "n/a";
    float point_margin = 0.0F;
    bool official_gesture_veto = false;
};

class HandAi {
public:
    ~HandAi();

    bool init();
    HandObservation infer(uint8_t *rgb888, uint16_t width, uint16_t height);

private:
    StaticGesture map_label(const char *label) const;
    bool category_supported(const char *label) const;

    HandDetect *detector_ = nullptr;
    HandGestureCls *classifier_ = nullptr;
    PointDirectionAi *point_direction_ = nullptr;
    bool ok_supported_ = false;
    bool open_palm_supported_ = false;
    bool thumb_up_supported_ = false;
    OfficialGestureEntryGrace entry_grace_;
};
