#include "gesture_engine.hpp"

#include <algorithm>

namespace {
bool static_action_details(StaticGesture gesture, int &action_id, int64_t &hold_ms)
{
    switch (gesture) {
    case StaticGesture::OK:
        action_id = 3;
        hold_ms = vision_config::kOkHoldMs;
        return true;
    case StaticGesture::OPEN_PALM:
        action_id = 4;
        hold_ms = vision_config::kOpenPalmHoldMs;
        return true;
    case StaticGesture::THUMB_UP:
        action_id = 5;
        hold_ms = vision_config::kThumbUpHoldMs;
        return true;
    case StaticGesture::POINT_RIGHT:
        action_id = 1;
        hold_ms = 0;
        return true;
    case StaticGesture::POINT_LEFT:
        action_id = 2;
        hold_ms = 0;
        return true;
    default:
        action_id = 0;
        hold_ms = 0;
        return false;
    }
}
} // namespace

const char *session_state_name(SessionState state)
{
    switch (state) {
    case SessionState::IDLE:
        return "IDLE";
    case SessionState::RECORDING:
        return "RECORDING";
    case SessionState::PAUSED:
        return "PAUSED";
    case SessionState::FINISHED:
        return "FINISHED";
    case SessionState::SCORE_REQUESTED:
        return "SCORE_REQUESTED";
    case SessionState::ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

const char *gesture_reason_name(GestureReason reason)
{
    switch (reason) {
    case GestureReason::WAITING_FOR_FRAME:
        return "waiting_for_frame";
    case GestureReason::NO_HAND:
        return "no_hand";
    case GestureReason::OUTSIDE_CONTROL_ZONE:
        return "outside_control_zone";
    case GestureReason::LOW_CONFIDENCE:
        return "low_confidence";
    case GestureReason::NON_ACTION_GESTURE:
        return "non_action_gesture";
    case GestureReason::COOLDOWN:
        return "cooldown";
    case GestureReason::STABILITY:
        return "stability";
    case GestureReason::HOLD:
        return "hold";
    case GestureReason::ACTION_TRIGGERED:
        return "action_triggered";
    default:
        return "unknown";
    }
}

GestureDecision GestureEngine::update(const HandObservation &observation, int64_t now_ms)
{
    GestureDecision decision;
    if (!observation.detected) {
        release_seen_ = true;
        reset_observation_history();
        if (!armed_ && now_ms >= cooldown_until_ms_) {
            armed_ = true;
        }
        decision.armed = armed_;
        decision.reason = GestureReason::NO_HAND;
        return decision;
    }
    if (!observation.in_control_zone) {
        release_seen_ = true;
        reset_observation_history();
        if (!armed_ && now_ms >= cooldown_until_ms_) {
            armed_ = true;
        }
        decision.armed = armed_;
        decision.reason = GestureReason::OUTSIDE_CONTROL_ZONE;
        return decision;
    }

    if (!armed_ && last_triggered_gesture_ != StaticGesture::UNKNOWN &&
        observation.gesture != StaticGesture::UNKNOWN && observation.gesture != StaticGesture::OTHER &&
        observation.gesture != last_triggered_gesture_) {
        release_seen_ = true;
    }
    if (!armed_ && release_seen_ && now_ms >= cooldown_until_ms_) {
        armed_ = true;
        reset_observation_history();
    }

    const StaticGesture classified = observation.gesture_confidence >= vision_config::kGestureConfidence
                                         ? observation.gesture
                                         : StaticGesture::UNKNOWN;
    append_classification(classified);

    if (!armed_) {
        decision.armed = false;
        decision.reason = GestureReason::COOLDOWN;
        return decision;
    }
    decision.armed = true;

    // The official Top-K guard is allowed to veto a custom point result even
    // below the normal action confidence.
    if (observation.official_gesture_veto) {
        candidate_gesture_ = StaticGesture::UNKNOWN;
        candidate_since_ms_ = 0;
        decision.reason = GestureReason::NON_ACTION_GESTURE;
        return decision;
    }

    if (observation.gesture_confidence < vision_config::kGestureConfidence) {
        candidate_gesture_ = StaticGesture::UNKNOWN;
        candidate_since_ms_ = 0;
        decision.reason = GestureReason::LOW_CONFIDENCE;
        return decision;
    }
    if (observation.gesture == StaticGesture::UNKNOWN || observation.gesture == StaticGesture::OTHER) {
        candidate_gesture_ = StaticGesture::UNKNOWN;
        candidate_since_ms_ = 0;
        decision.reason = GestureReason::NON_ACTION_GESTURE;
        return decision;
    }

    int action_id = 0;
    int64_t hold_required_ms = 0;
    if (!static_action_details(observation.gesture, action_id, hold_required_ms)) {
        decision.reason = GestureReason::NON_ACTION_GESTURE;
        return decision;
    }

    if (candidate_gesture_ != observation.gesture) {
        candidate_gesture_ = observation.gesture;
        candidate_since_ms_ = now_ms;
    }

    decision.votes = classification_votes(candidate_gesture_);
    decision.hold_required_ms = hold_required_ms;
    decision.hold_elapsed_ms = std::max<int64_t>(0, now_ms - candidate_since_ms_);
    if (decision.votes < decision.votes_required) {
        decision.reason = GestureReason::STABILITY;
        return decision;
    }
    if (decision.hold_elapsed_ms < decision.hold_required_ms) {
        decision.reason = GestureReason::HOLD;
        return decision;
    }

    latch_action(classified, now_ms);
    decision.action_id = action_id;
    decision.reason = GestureReason::ACTION_TRIGGERED;
    decision.armed = false;
    return decision;
}

void GestureEngine::reset_observation_history()
{
    history_count_ = 0;
    history_write_index_ = 0;
    candidate_gesture_ = StaticGesture::UNKNOWN;
    candidate_since_ms_ = 0;
}

void GestureEngine::append_classification(StaticGesture gesture)
{
    history_[history_write_index_] = gesture;
    history_write_index_ = (history_write_index_ + 1) % vision_config::kClassificationHistorySize;
    history_count_ = std::min(history_count_ + 1, vision_config::kClassificationHistorySize);
}

size_t GestureEngine::classification_votes(StaticGesture gesture) const
{
    size_t votes = 0;
    for (size_t index = 0; index < history_count_; ++index) {
        if (history_[index] == gesture) {
            ++votes;
        }
    }
    return votes;
}

void GestureEngine::latch_action(StaticGesture gesture, int64_t now_ms)
{
    armed_ = false;
    release_seen_ = false;
    cooldown_until_ms_ = now_ms + vision_config::kActionCooldownMs;
    last_triggered_gesture_ = gesture;
    reset_observation_history();
}
