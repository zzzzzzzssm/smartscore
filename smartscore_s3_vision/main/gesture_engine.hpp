#pragma once

#include <cstddef>
#include <cstdint>

#include "hand_ai.hpp"
#include "vision_config.hpp"

enum class SessionState {
    IDLE = 0,
    RECORDING,
    PAUSED,
    FINISHED,
    SCORE_REQUESTED,
    ERROR,
};

enum class GestureReason : uint8_t {
    WAITING_FOR_FRAME = 0,
    NO_HAND,
    OUTSIDE_CONTROL_ZONE,
    LOW_CONFIDENCE,
    NON_ACTION_GESTURE,
    COOLDOWN,
    STABILITY,
    HOLD,
    ACTION_TRIGGERED,
};

struct GestureDecision {
    int action_id = 0;
    GestureReason reason = GestureReason::WAITING_FOR_FRAME;
    size_t votes = 0;
    size_t votes_required = vision_config::kClassificationVotesRequired;
    int64_t hold_elapsed_ms = 0;
    int64_t hold_required_ms = 0;
    bool armed = true;
};

const char *session_state_name(SessionState state);
const char *gesture_reason_name(GestureReason reason);

class GestureEngine {
public:
    GestureDecision update(const HandObservation &observation, int64_t now_ms);

private:
    void reset_observation_history();
    void append_classification(StaticGesture gesture);
    size_t classification_votes(StaticGesture gesture) const;
    void latch_action(StaticGesture gesture, int64_t now_ms);

    StaticGesture history_[vision_config::kClassificationHistorySize] = {};
    size_t history_count_ = 0;
    size_t history_write_index_ = 0;
    StaticGesture candidate_gesture_ = StaticGesture::UNKNOWN;
    int64_t candidate_since_ms_ = 0;
    bool armed_ = true;
    bool release_seen_ = false;
    int64_t cooldown_until_ms_ = 0;
    StaticGesture last_triggered_gesture_ = StaticGesture::UNKNOWN;
};
