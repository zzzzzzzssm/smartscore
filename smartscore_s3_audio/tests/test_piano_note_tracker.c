#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "piano_note_tracker.h"

static int s_failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n",                 \
                    __FILE__, __LINE__, #condition);                         \
            ++s_failures;                                                    \
        }                                                                    \
    } while (0)

static chord_result_t spectrum_with(const int *midis, size_t count)
{
    chord_result_t spectrum;
    memset(&spectrum, 0, sizeof(spectrum));
    for (size_t index = 0; index < count; ++index) {
        spectrum.key_salience[midis[index] - PIANO_TRACKER_MIDI_MIN] =
            0.90f - 0.05f * (float)index;
    }
    spectrum.supported_note_count = (int)count;
    spectrum.too_many_notes = count > PIANO_NOTE_SET_MAX_KEYS;
    return spectrum;
}

static void test_attack_release_and_four_key_sorting(void)
{
    piano_note_tracker_t tracker;
    piano_note_tracker_init(&tracker);
    piano_note_set_t set;
    const int notes[] = {60, 36, 96, 49};
    chord_result_t spectrum = spectrum_with(notes, 4);
    yin_result_t no_yin = {.midi = -1};

    CHECK(piano_note_tracker_update(&tracker, &spectrum, &no_yin, &no_yin,
                                    true, false, 0.02f, 100, &set));
    CHECK(set.count == 0); /* initial mic-health snapshot only */
    CHECK(piano_note_tracker_update(&tracker, &spectrum, &no_yin, &no_yin,
                                    true, false, 0.02f, 185, &set));
    CHECK(set.count == 4);
    CHECK(set.midi[0] == 36 && set.midi[1] == 49 &&
          set.midi[2] == 60 && set.midi[3] == 96);
    const uint8_t held_velocity = set.velocity[0];
    const float held_confidence = set.confidence[0];

    const int retained[] = {36, 49, 60};
    chord_result_t retained_spectrum = spectrum_with(retained, 3);
    CHECK(!piano_note_tracker_update(&tracker, &retained_spectrum,
                                     &no_yin, &no_yin, true, false,
                                     0.001f, 270, &set));
    CHECK(set.count == 4);
    CHECK(piano_note_tracker_update(&tracker, &retained_spectrum,
                                    &no_yin, &no_yin, true, false,
                                    0.001f, 355, &set));
    CHECK(set.count == 3);
    CHECK(set.velocity[0] == held_velocity);
    CHECK(set.confidence[0] == held_confidence);

    chord_result_t silence = {0};
    CHECK(!piano_note_tracker_update(&tracker, &silence, &no_yin, &no_yin,
                                     false, false, 0.0f, 440, &set));
    CHECK(set.count == 3);
    CHECK(piano_note_tracker_update(&tracker, &silence, &no_yin, &no_yin,
                                    false, false, 0.0f, 525, &set));
    CHECK(set.count == 0);
}

static void test_low_yin_and_overflow_hold(void)
{
    piano_note_tracker_t tracker;
    piano_note_tracker_init(&tracker);
    piano_note_set_t set;
    chord_result_t empty = {0};
    yin_result_t low = {
        .valid = true,
        .midi = 36,
        .confidence = 0.91f,
    };
    yin_result_t no_yin = {.midi = -1};
    (void)piano_note_tracker_update(&tracker, &empty, &no_yin, &low,
                                    true, true, 0.01f, 100, &set);
    CHECK(piano_note_tracker_update(&tracker, &empty, &no_yin, &low,
                                    true, true, 0.01f, 185, &set));
    CHECK(set.count == 1 && set.midi[0] == 36 && set.degraded_mic);

    const int five[] = {36, 40, 43, 47, 52};
    chord_result_t overflow = spectrum_with(five, 5);
    CHECK(!piano_note_tracker_update(&tracker, &overflow, &no_yin, &no_yin,
                                     true, true, 0.02f, 270, &set));
    CHECK(set.overflow);
    CHECK(set.count == 1 && set.midi[0] == 36);
}

static void test_virtual_low_fundamental_requires_yin_confirmation(void)
{
    piano_note_tracker_t tracker;
    piano_note_tracker_init(&tracker);
    piano_note_set_t set;
    chord_result_t virtual_low = {0};
    virtual_low.key_salience[0] = 0.90f;
    virtual_low.key_uses_virtual_fundamental[0] = true;
    yin_result_t no_yin = {.midi = -1};

    (void)piano_note_tracker_update(&tracker, &virtual_low, &no_yin, &no_yin,
                                    true, false, 0.01f, 100, &set);
    (void)piano_note_tracker_update(&tracker, &virtual_low, &no_yin, &no_yin,
                                    true, false, 0.01f, 185, &set);
    CHECK(set.count == 0);

    yin_result_t low = {
        .valid = true,
        .midi = 36,
        .confidence = 0.91f,
    };
    (void)piano_note_tracker_update(&tracker, &virtual_low, &no_yin, &low,
                                    true, false, 0.01f, 270, &set);
    CHECK(piano_note_tracker_update(&tracker, &virtual_low, &no_yin, &low,
                                    true, false, 0.01f, 355, &set));
    CHECK(set.count == 1 && set.midi[0] == 36);
}

int main(void)
{
    test_attack_release_and_four_key_sorting();
    test_low_yin_and_overflow_hold();
    test_virtual_low_fundamental_requires_yin_confirmation();
    if (s_failures != 0) {
        fprintf(stderr, "%d piano tracker test(s) failed\n", s_failures);
        return 1;
    }
    puts("piano note tracker tests passed");
    return 0;
}
