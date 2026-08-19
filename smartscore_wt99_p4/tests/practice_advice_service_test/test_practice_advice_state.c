#include <stdio.h>
#include <stdlib.h>

#include "practice_advice_state.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #condition);                                   \
            return EXIT_FAILURE;                                             \
        }                                                                    \
    } while (0)

int main(void)
{
    practice_advice_core_state_t state;
    practice_advice_state_init(&state);
    CHECK(state.generation == 0);
    CHECK(state.phase == PRACTICE_ADVICE_CORE_NONE);

    uint32_t first = practice_advice_state_begin(&state);
    CHECK(first != 0);
    CHECK(state.phase == PRACTICE_ADVICE_CORE_WAITING_SCORE);
    CHECK(practice_advice_state_mark_running(&state, first));
    CHECK(!practice_advice_state_mark_running(&state, first));
    CHECK(practice_advice_state_finish(&state, first,
                                       PRACTICE_ADVICE_CORE_READY));
    CHECK(!practice_advice_state_finish(&state, first,
                                        PRACTICE_ADVICE_CORE_FAILED));

    uint32_t second = practice_advice_state_begin(&state);
    CHECK(second != first);
    CHECK(!practice_advice_state_finish(&state, first,
                                        PRACTICE_ADVICE_CORE_READY));
    CHECK(practice_advice_state_finish(
        &state, second, PRACTICE_ADVICE_CORE_SKIPPED_OFFLINE));

    uint32_t third = practice_advice_state_begin(&state);
    CHECK(practice_advice_state_finish(&state, third,
                                       PRACTICE_ADVICE_CORE_FAILED));

    uint32_t fourth = practice_advice_state_begin(&state);
    CHECK(practice_advice_state_mark_running(&state, fourth));
    CHECK(practice_advice_state_finish(&state, fourth,
                                       PRACTICE_ADVICE_CORE_FAILED));
    CHECK(practice_advice_state_is_current(&state, fourth));
    CHECK(!practice_advice_state_is_current(&state, third));

    puts("practice advice state tests passed");
    return EXIT_SUCCESS;
}
