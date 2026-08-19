#include "practice_advice_state.h"

#include <stddef.h>

void practice_advice_state_init(practice_advice_core_state_t *state)
{
    if (state == NULL) return;
    state->generation = 0;
    state->phase = PRACTICE_ADVICE_CORE_NONE;
}

uint32_t practice_advice_state_begin(practice_advice_core_state_t *state)
{
    if (state == NULL) return 0;
    ++state->generation;
    if (state->generation == 0) ++state->generation;
    state->phase = PRACTICE_ADVICE_CORE_WAITING_SCORE;
    return state->generation;
}

bool practice_advice_state_mark_running(practice_advice_core_state_t *state,
                                        uint32_t generation)
{
    if (state == NULL || state->generation != generation ||
        state->phase != PRACTICE_ADVICE_CORE_WAITING_SCORE) {
        return false;
    }
    state->phase = PRACTICE_ADVICE_CORE_RUNNING;
    return true;
}

bool practice_advice_state_finish(practice_advice_core_state_t *state,
                                  uint32_t generation,
                                  practice_advice_core_phase_t terminal_phase)
{
    if (state == NULL || state->generation != generation) return false;

    const bool from_waiting =
        state->phase == PRACTICE_ADVICE_CORE_WAITING_SCORE;
    const bool from_running = state->phase == PRACTICE_ADVICE_CORE_RUNNING;
    const bool valid =
        (terminal_phase == PRACTICE_ADVICE_CORE_SKIPPED_OFFLINE &&
         from_waiting) ||
        (terminal_phase == PRACTICE_ADVICE_CORE_READY && from_running) ||
        (terminal_phase == PRACTICE_ADVICE_CORE_FAILED &&
         (from_waiting || from_running));
    if (!valid) return false;
    state->phase = terminal_phase;
    return true;
}

bool practice_advice_state_is_current(const practice_advice_core_state_t *state,
                                      uint32_t generation)
{
    return state != NULL && generation != 0 &&
           state->generation == generation;
}
