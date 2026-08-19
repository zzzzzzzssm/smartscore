#pragma once

#include <cstdint>

#include "esp_err.h"

class SdStorage;

enum class P4ControlCommand : uint8_t {
    START_PRACTICE = 1,
    PAUSE_PRACTICE = 2,
    NEXT_PAGE = 3,
    PREVIOUS_PAGE = 4,
    SHOW_SCORE = 6,
};

struct P4ControlAck {
    P4ControlCommand command = P4ControlCommand::START_PRACTICE;
    bool handled = false;
};

enum class P4PracticeState : uint8_t {
    PLAYING = 0,
    PAUSED,
    FINISHED,
};

struct P4PracticeStateEvent {
    P4PracticeState state = P4PracticeState::FINISHED;
    char session_id[64] = {};
};

/** Initialize the dedicated bidirectional UART link to the WT99 P4. */
esp_err_t p4_control_link_init();

/** Register the SD photo provider used by on-demand P4 photo requests. */
void p4_control_link_set_photo_storage(SdStorage *storage);

/** Send one newline-terminated gesture command frame to the WT99 P4. */
esp_err_t p4_control_link_send(P4ControlCommand command);

/** Poll one P4 acknowledgement without blocking. */
bool p4_control_link_receive_ack(P4ControlAck &ack);

/** Poll the latest P4 practice state without blocking. */
bool p4_control_link_receive_practice_state(P4PracticeStateEvent &event);
