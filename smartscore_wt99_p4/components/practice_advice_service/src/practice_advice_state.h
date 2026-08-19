#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PRACTICE_ADVICE_CORE_NONE = 0,
    PRACTICE_ADVICE_CORE_WAITING_SCORE,
    PRACTICE_ADVICE_CORE_RUNNING,
    PRACTICE_ADVICE_CORE_READY,
    PRACTICE_ADVICE_CORE_SKIPPED_OFFLINE,
    PRACTICE_ADVICE_CORE_FAILED,
} practice_advice_core_phase_t;

typedef struct {
    uint32_t generation;
    practice_advice_core_phase_t phase;
} practice_advice_core_state_t;

void practice_advice_state_init(practice_advice_core_state_t *state);
uint32_t practice_advice_state_begin(practice_advice_core_state_t *state);
bool practice_advice_state_mark_running(practice_advice_core_state_t *state,
                                        uint32_t generation);
bool practice_advice_state_finish(practice_advice_core_state_t *state,
                                  uint32_t generation,
                                  practice_advice_core_phase_t terminal_phase);
bool practice_advice_state_is_current(const practice_advice_core_state_t *state,
                                      uint32_t generation);
