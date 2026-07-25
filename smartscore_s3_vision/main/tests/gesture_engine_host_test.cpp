#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

#include "../gesture_engine.hpp"
#include "../official_gesture_entry_grace.hpp"
#include "../official_gesture_guard.hpp"

static_assert(vision_config::kPointDirectionHandConfidence == 0.35F);
static_assert(vision_config::kPointDirectionConfidence == 0.55F);
static_assert(vision_config::kPointDirectionMargin == 0.20F);
static_assert(vision_config::kClassificationVotesRequired == 1);

namespace {
struct OfficialCandidate {
    const char *cat_name;
    float score;
};

HandObservation gesture(StaticGesture value, float confidence)
{
    HandObservation observation;
    observation.detected = true;
    observation.hand_count = 1;
    observation.in_control_zone = true;
    observation.gesture = value;
    observation.gesture_confidence = confidence;
    observation.detection_confidence = 0.75F;
    return observation;
}

void test_point_actions_trigger_on_one_result()
{
    GestureEngine right;
    GestureEngine left;
    assert(right.update(gesture(StaticGesture::POINT_RIGHT, 0.95F), 1000).action_id == 1);
    assert(left.update(gesture(StaticGesture::POINT_LEFT, 0.95F), 1000).action_id == 2);
}

void test_low_confidence_does_not_trigger()
{
    GestureEngine engine;
    const GestureDecision decision = engine.update(gesture(StaticGesture::POINT_RIGHT, 0.54F), 1000);
    assert(decision.action_id == 0);
    assert(decision.reason == GestureReason::LOW_CONFIDENCE);
}

void test_official_veto_wins_before_action_confidence()
{
    GestureEngine engine;
    HandObservation observation = gesture(StaticGesture::POINT_LEFT, 0.95F);
    observation.official_gesture_veto = true;
    const GestureDecision decision = engine.update(observation, 1000);
    assert(decision.action_id == 0);
    assert(decision.reason == GestureReason::NON_ACTION_GESTURE);
}

void test_official_actions_remain_single_result()
{
    GestureEngine ok;
    GestureEngine palm;
    GestureEngine thumb;
    assert(ok.update(gesture(StaticGesture::OK, 0.95F), 1000).action_id == 3);
    assert(palm.update(gesture(StaticGesture::OPEN_PALM, 0.95F), 1000).action_id == 4);
    assert(thumb.update(gesture(StaticGesture::THUMB_UP, 0.95F), 1000).action_id == 5);
}

void test_trigger_does_not_repeat_while_held()
{
    GestureEngine engine;
    assert(engine.update(gesture(StaticGesture::POINT_RIGHT, 0.95F), 1000).action_id == 1);
    assert(engine.update(gesture(StaticGesture::POINT_RIGHT, 0.96F), 2000).action_id == 0);
}

void test_release_allows_next_action_after_cooldown()
{
    GestureEngine engine;
    assert(engine.update(gesture(StaticGesture::POINT_RIGHT, 0.95F), 1000).action_id == 1);
    HandObservation no_hand;
    assert(engine.update(no_hand, 1900).action_id == 0);
    assert(engine.update(gesture(StaticGesture::POINT_RIGHT, 0.95F), 2000).action_id == 1);
}

void test_official_candidate_veto()
{
    const std::vector<OfficialCandidate> candidates = {
        {"one", 0.60F}, {"like", 0.30F}, {"no_gesture", 0.10F}};
    const OfficialCandidate *protected_candidate =
        official_gesture_guard::find_protected_candidate(candidates, 0.25F);
    assert(protected_candidate != nullptr);
    assert(std::strcmp(protected_candidate->cat_name, "like") == 0);
    assert(std::strcmp(official_gesture_guard::veto_label(protected_candidate->cat_name),
                       "official_veto_like") == 0);

    const std::vector<OfficialCandidate> low_score = {{"like", 0.24F}, {"one", 0.70F}};
    assert(official_gesture_guard::find_protected_candidate(low_score, 0.25F) == nullptr);

    const std::vector<OfficialCandidate> unrelated = {
        {"one", 0.70F}, {"two", 0.20F}, {"no_gesture", 0.10F}};
    assert(official_gesture_guard::find_protected_candidate(unrelated, 0.25F) == nullptr);
}

void test_official_entry_grace_is_one_frame_and_resets()
{
    OfficialGestureEntryGrace grace;
    assert(grace.pending());
    assert(grace.consume());
    assert(!grace.pending());
    assert(!grace.consume());

    grace.reset();
    assert(grace.pending());
    assert(grace.consume());
    assert(!grace.consume());
}

} // namespace

int main()
{
    test_point_actions_trigger_on_one_result();
    test_low_confidence_does_not_trigger();
    test_official_veto_wins_before_action_confidence();
    test_official_actions_remain_single_result();
    test_trigger_does_not_repeat_while_held();
    test_release_allows_next_action_after_cooldown();
    test_official_candidate_veto();
    test_official_entry_grace_is_one_frame_and_resets();
    std::cout << "gesture_engine_host_test: OK\n";
    return 0;
}
