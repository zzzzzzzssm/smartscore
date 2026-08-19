#include "unity.h"

#include "creator_quantizer.h"
#include "creator_recorder.h"
#include "creator_snapshot_mailbox.h"

/* midi_data_t intentionally retains raw performance events and is too large
 * for the small Unity task stack. Reuse static workspaces across cases. */
static midi_data_t snapshot;
static midi_data_t live;
static midi_data_t live_snapshot;

static creator_recorder_config_t grand_staff_config(void)
{
    return (creator_recorder_config_t){
        .bpm = 120,
        .time_sig_num = 4,
        .time_sig_den = 4,
        .staff_mode = CREATOR_STAFF_GRAND,
        .duration_tolerance_percent = 30,
        .grand_staff_split_note = 60,
    };
}

TEST_CASE("creator duration quantizer recognizes standard note values",
          "[creator]")
{
    TEST_ASSERT_EQUAL_UINT32(1920,
        creator_quantize_duration(2000000, 120, 30).ticks);
    TEST_ASSERT_EQUAL_UINT32(960,
        creator_quantize_duration(1000000, 120, 30).ticks);
    TEST_ASSERT_EQUAL_UINT32(480,
        creator_quantize_duration(500000, 120, 30).ticks);
    TEST_ASSERT_EQUAL_UINT32(240,
        creator_quantize_duration(250000, 120, 30).ticks);
    TEST_ASSERT_EQUAL_UINT32(120,
        creator_quantize_duration(125000, 120, 30).ticks);
}

TEST_CASE("creator duration quantizer recognizes dotted note values",
          "[creator][quantizer]")
{
    creator_quantized_duration_t dotted_quarter =
        creator_quantize_duration(750000, 120, 20);
    TEST_ASSERT_EQUAL_UINT32(720, dotted_quarter.ticks);
    TEST_ASSERT_EQUAL_UINT8(1, dotted_quarter.dots);
    TEST_ASSERT_TRUE(dotted_quarter.within_tolerance);

    creator_quantized_duration_t dotted_eighth =
        creator_quantize_duration(375000, 120, 20);
    TEST_ASSERT_EQUAL_UINT32(360, dotted_eighth.ticks);
    TEST_ASSERT_EQUAL_UINT8(1, dotted_eighth.dots);

    creator_quantized_duration_t multi_bar =
        creator_quantize_duration(5000000, 120, 20);
    TEST_ASSERT_EQUAL_UINT32(4800, multi_bar.ticks);
    TEST_ASSERT_EQUAL_UINT8(0, multi_bar.dots);
}

TEST_CASE("creator records Note On Off timing and piano staff mapping",
          "[creator]")
{
    TEST_ASSERT_EQUAL(ESP_OK, creator_recorder_init());
    creator_recorder_config_t config = grand_staff_config();
    creator_recorder_begin(&config, 1000000);

    TEST_ASSERT_TRUE(creator_recorder_note_on(0, 64, 90, 1000000));
    TEST_ASSERT_TRUE(creator_recorder_snapshot_live(&live_snapshot, 1000000));
    TEST_ASSERT_EQUAL_INT(1, live_snapshot.note_count);
    TEST_ASSERT_EQUAL_UINT8(64, live_snapshot.notes[0].note);
    TEST_ASSERT_EQUAL_UINT32(120, live_snapshot.notes[0].duration);

    TEST_ASSERT_TRUE(creator_recorder_note_off(0, 64, 1500000));
    TEST_ASSERT_TRUE(creator_recorder_note_on(0, 48, 80, 1500000));
    TEST_ASSERT_TRUE(creator_recorder_note_off(0, 48, 2000000));

    creator_recorded_note_t first;
    TEST_ASSERT_TRUE(creator_recorder_get_note(0, &first));
    TEST_ASSERT_EQUAL_UINT8(64, first.pitch);
    TEST_ASSERT_EQUAL_UINT64(1000000, first.start_time_us);
    TEST_ASSERT_EQUAL_UINT64(1500000, first.end_time_us);
    TEST_ASSERT_EQUAL_UINT64(500000, first.duration_us);
    TEST_ASSERT_EQUAL_UINT32(480, first.duration_ticks);
    TEST_ASSERT_EQUAL_UINT8(1, first.staff);

    TEST_ASSERT_TRUE(creator_recorder_snapshot(&snapshot));
    TEST_ASSERT_EQUAL_INT(2, snapshot.note_count);
    TEST_ASSERT_EQUAL_UINT8(2, snapshot.notes[1].staff);
}

TEST_CASE("creator starts each recording segment on its first note",
          "[creator]")
{
    creator_recorder_config_t config = grand_staff_config();
    creator_recorder_begin(&config, 1000000);

    /* Waiting after pressing Start must not create leading empty beats. */
    TEST_ASSERT_TRUE(creator_recorder_note_on(0, 60, 90, 6000000));
    TEST_ASSERT_TRUE(creator_recorder_note_off(0, 60, 6500000));
    creator_recorded_note_t first;
    TEST_ASSERT_TRUE(creator_recorder_get_note(0, &first));
    TEST_ASSERT_EQUAL_UINT32(0, first.start_tick);

    creator_recorder_pause(7000000);
    creator_recorder_resume(8000000);

    /* Resume is armed too; the delay before the next note is not musical
     * time, so the new note starts exactly at the append position. */
    TEST_ASSERT_TRUE(creator_recorder_note_on(0, 62, 90, 13000000));
    TEST_ASSERT_TRUE(creator_recorder_note_off(0, 62, 13500000));
    TEST_ASSERT_TRUE(creator_recorder_snapshot(&snapshot));
    TEST_ASSERT_EQUAL_INT(2, snapshot.note_count);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.notes[0].start_tick);
    TEST_ASSERT_EQUAL_UINT32(480, snapshot.notes[1].start_tick);
}

TEST_CASE("creator overwrite removes selected measure and later notes",
          "[creator]")
{
    creator_recorder_config_t config = grand_staff_config();
    creator_recorder_begin(&config, 0);

    creator_recorder_note_on(0, 60, 90, 0);
    creator_recorder_note_off(0, 60, 500000);
    creator_recorder_note_on(0, 65, 90, 2000000);
    creator_recorder_note_off(0, 65, 2500000);
    creator_recorder_pause(3000000);

    TEST_ASSERT_EQUAL_INT(2, creator_recorder_measure_count());
    TEST_ASSERT_TRUE(creator_recorder_resume_from_measure(2, 4000000));
    TEST_ASSERT_EQUAL_INT(1, creator_recorder_note_count());

    /* Waiting after selecting an overwrite measure must not move its start. */
    creator_recorder_note_on(0, 67, 90, 7000000);
    creator_recorder_note_off(0, 67, 7500000);

    creator_recorder_snapshot(&snapshot);
    TEST_ASSERT_EQUAL_INT(2, snapshot.note_count);
    TEST_ASSERT_EQUAL_UINT8(60, snapshot.notes[0].note);
    TEST_ASSERT_EQUAL_UINT8(67, snapshot.notes[1].note);
    TEST_ASSERT_EQUAL_UINT32(1920, snapshot.notes[1].start_tick);
}

TEST_CASE("creator replaces a selected measure range and auto pauses",
          "[creator]")
{
    creator_recorder_config_t config = grand_staff_config();
    creator_recorder_begin(&config, 0);
    creator_recorder_note_on(0, 60, 90, 0);
    creator_recorder_note_off(0, 60, 500000);
    creator_recorder_note_on(0, 62, 90, 2000000);
    creator_recorder_note_off(0, 62, 2500000);
    creator_recorder_note_on(0, 64, 90, 4000000);
    creator_recorder_note_off(0, 64, 4500000);
    creator_recorder_note_on(0, 65, 90, 6000000);
    creator_recorder_note_off(0, 65, 6500000);
    creator_recorder_pause(7000000);

    TEST_ASSERT_TRUE(creator_recorder_resume_measure_range(2, 3, 10000000));
    TEST_ASSERT_EQUAL_INT(2, creator_recorder_note_count());
    TEST_ASSERT_TRUE(creator_recorder_waiting_for_first_note());

    /* The selected range starts on its first new note, not on the tap time. */
    TEST_ASSERT_TRUE(creator_recorder_note_on(0, 67, 90, 20000000));
    TEST_ASSERT_TRUE(creator_recorder_note_off(0, 67, 20500000));

    /* Four seconds at 120 BPM is exactly two 4/4 measures. Polling pauses at
     * the range end even when the final portion contains no MIDI events. */
    TEST_ASSERT_TRUE(creator_recorder_poll(24000000));
    TEST_ASSERT_EQUAL(CREATOR_RECORDER_PAUSED, creator_recorder_state());

    TEST_ASSERT_TRUE(creator_recorder_snapshot(&snapshot));
    TEST_ASSERT_EQUAL_INT(3, snapshot.note_count);
    TEST_ASSERT_EQUAL_UINT8(60, snapshot.notes[0].note);
    TEST_ASSERT_EQUAL_UINT8(67, snapshot.notes[1].note);
    TEST_ASSERT_EQUAL_UINT32(1920, snapshot.notes[1].start_tick);
    TEST_ASSERT_EQUAL_UINT8(65, snapshot.notes[2].note);
    TEST_ASSERT_EQUAL_UINT32(5760, snapshot.notes[2].start_tick);

    /* The normal Continue action appends after the preserved song ending. */
    creator_recorder_resume(25000000);
    TEST_ASSERT_TRUE(creator_recorder_waiting_for_first_note());
    TEST_ASSERT_TRUE(creator_recorder_note_on(0, 71, 90, 30000000));
    TEST_ASSERT_TRUE(creator_recorder_note_off(0, 71, 30500000));
    TEST_ASSERT_TRUE(creator_recorder_snapshot(&snapshot));
    TEST_ASSERT_EQUAL_UINT32(6240, snapshot.notes[3].start_tick);
}

TEST_CASE("creator single staff keeps low notes on upper staff", "[creator]")
{
    creator_recorder_config_t config = grand_staff_config();
    config.staff_mode = CREATOR_STAFF_SINGLE;
    creator_recorder_begin(&config, 0);
    creator_recorder_note_on(0, 40, 75, 0);
    creator_recorder_note_off(0, 40, 500000);

    creator_recorder_snapshot(&snapshot);
    TEST_ASSERT_EQUAL_INT(1, snapshot.note_count);
    TEST_ASSERT_EQUAL_UINT8(1, snapshot.notes[0].staff);
    TEST_ASSERT_EQUAL_UINT8(1, snapshot.notes[0].voice);
}

TEST_CASE("creator limits burst input without breaking chords", "[creator]")
{
    creator_recorder_config_t config = grand_staff_config();
    config.chord_window_ms = 45;
    config.max_notes_per_onset = 2;
    creator_recorder_begin(&config, 0);

    TEST_ASSERT_TRUE(creator_recorder_note_on(0, 60, 90, 0));
    TEST_ASSERT_TRUE(creator_recorder_note_on(0, 64, 90, 20000));
    TEST_ASSERT_FALSE(creator_recorder_note_on(0, 67, 90, 30000));
    TEST_ASSERT_FALSE(creator_recorder_note_on(0, 69, 90, 70000));

    TEST_ASSERT_TRUE(creator_recorder_snapshot_live(&live, 80000));
    TEST_ASSERT_EQUAL_INT(2, live.note_count);
    TEST_ASSERT_EQUAL_UINT32(0, live.notes[0].start_tick);
    TEST_ASSERT_EQUAL_UINT32(0, live.notes[1].start_tick);

    /* Suppressed Note Off messages must not truncate accepted notes. */
    TEST_ASSERT_FALSE(creator_recorder_note_off(0, 67, 90000));
    TEST_ASSERT_FALSE(creator_recorder_note_off(0, 69, 90000));
    TEST_ASSERT_TRUE(creator_recorder_note_off(0, 60, 125000));
    TEST_ASSERT_TRUE(creator_recorder_note_off(0, 64, 125000));

    TEST_ASSERT_TRUE(creator_recorder_note_on(0, 67, 90, 130000));
    TEST_ASSERT_TRUE(creator_recorder_note_off(0, 67, 255000));

    TEST_ASSERT_TRUE(creator_recorder_snapshot(&snapshot));
    TEST_ASSERT_EQUAL_INT(3, snapshot.note_count);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.notes[0].start_tick);
    TEST_ASSERT_EQUAL_UINT32(0, snapshot.notes[1].start_tick);
    TEST_ASSERT_EQUAL_UINT32(120, snapshot.notes[2].start_tick);
}

TEST_CASE("creator splits a sustained note at the barline with persistent tie",
          "[creator][tie]")
{
    creator_recorder_config_t config = grand_staff_config();
    creator_recorder_begin(&config, 0);
    TEST_ASSERT_TRUE(creator_recorder_note_on(0, 60, 90, 0));
    TEST_ASSERT_TRUE(creator_recorder_note_off(0, 60, 500000));
    TEST_ASSERT_TRUE(creator_recorder_note_on(0, 62, 90, 1875000));
    TEST_ASSERT_TRUE(creator_recorder_note_off(0, 62, 2375000));

    TEST_ASSERT_TRUE(creator_recorder_snapshot(&snapshot));
    TEST_ASSERT_EQUAL_INT(3, snapshot.note_count);
    TEST_ASSERT_EQUAL_UINT32(1800, snapshot.notes[1].start_tick);
    TEST_ASSERT_EQUAL_UINT32(120, snapshot.notes[1].duration);
    TEST_ASSERT_EQUAL_UINT8(MIDI_NOTE_TIE_START,
                            snapshot.notes[1].tie_flags);
    TEST_ASSERT_EQUAL_UINT32(1920, snapshot.notes[2].start_tick);
    TEST_ASSERT_EQUAL_UINT32(360, snapshot.notes[2].duration);
    TEST_ASSERT_EQUAL_UINT8(MIDI_NOTE_TIE_STOP,
                            snapshot.notes[2].tie_flags);
    TEST_ASSERT_EQUAL_UINT8(1, snapshot.notes[2].dots);
}

TEST_CASE("creator keeps explicit slur gliss pedal and pitch bend semantics",
          "[creator][connections][midi]")
{
    creator_recorder_config_t config = grand_staff_config();
    creator_recorder_begin(&config, 0);
    TEST_ASSERT_TRUE(creator_recorder_note_on(0, 60, 90, 0));
    TEST_ASSERT_TRUE(creator_recorder_control_change(0, 64, 127, 250000));
    TEST_ASSERT_TRUE(creator_recorder_note_off(0, 60, 500000));

    TEST_ASSERT_TRUE(creator_recorder_snapshot_live(&live, 750000));
    TEST_ASSERT_EQUAL_INT(1, live.note_count);
    TEST_ASSERT_EQUAL_UINT32(720, live.notes[0].duration);
    TEST_ASSERT_TRUE(creator_recorder_pitch_bend(0, 4096, 800000));
    TEST_ASSERT_TRUE(creator_recorder_control_change(0, 64, 0, 1000000));

    TEST_ASSERT_TRUE(creator_recorder_note_on(0, 64, 90, 1125000));
    TEST_ASSERT_TRUE(creator_recorder_note_off(0, 64, 1625000));
    TEST_ASSERT_TRUE(creator_recorder_note_on(0, 67, 90, 1750000));
    TEST_ASSERT_TRUE(creator_recorder_note_off(0, 67, 2250000));
    TEST_ASSERT_TRUE(creator_recorder_add_connection(
        CREATOR_CONNECTION_SLUR, 0, 1, 1));
    TEST_ASSERT_TRUE(creator_recorder_add_connection(
        CREATOR_CONNECTION_GLISS, 1, 2, 2));

    TEST_ASSERT_TRUE(creator_recorder_snapshot(&snapshot));
    TEST_ASSERT_EQUAL_UINT8(1, snapshot.notes[0].slur_start);
    TEST_ASSERT_EQUAL_UINT8(1, snapshot.notes[1].slur_stop);
    TEST_ASSERT_EQUAL_UINT8(2, snapshot.notes[1].gliss_start);
    TEST_ASSERT_EQUAL_UINT8(2, snapshot.notes[2].gliss_stop);
    TEST_ASSERT_EQUAL_INT(3, snapshot.raw_event_count);
    TEST_ASSERT_EQUAL(MIDI_RAW_CONTROL_CHANGE,
                      snapshot.raw_events[0].type);
    TEST_ASSERT_EQUAL(MIDI_RAW_PITCH_BEND,
                      snapshot.raw_events[1].type);
}

TEST_CASE("creator dense input keeps the final accepted state",
          "[creator][density]")
{
    creator_recorder_config_t config = grand_staff_config();
    config.max_notes_per_onset = 16;
    creator_recorder_begin(&config, 0);
    uint64_t timestamp = 0;
    for (int group = 0; group < 8; ++group) {
        for (int note = 0; note < 12; ++note)
            TEST_ASSERT_TRUE(creator_recorder_note_on(
                0, (uint8_t)(48 + note), 90, timestamp + note * 1000));
        for (int note = 0; note < 12; ++note)
            TEST_ASSERT_TRUE(creator_recorder_note_off(
                0, (uint8_t)(48 + note), timestamp + 125000));
        timestamp += 125000;
    }
    TEST_ASSERT_TRUE(creator_recorder_snapshot(&snapshot));
    TEST_ASSERT_EQUAL_INT(96, snapshot.note_count);
    TEST_ASSERT_EQUAL_UINT32(840, snapshot.notes[95].start_tick);
}

TEST_CASE("live snapshot mailbox collapses queue backlog to final state",
          "[creator][refresh][backlog]")
{
    creator_snapshot_mailbox_t mailbox = {0};
    for (uintptr_t sequence = 1; sequence <= 1000; ++sequence) {
        midi_data_t *frame = (midi_data_t *)sequence;
        midi_data_t *superseded = creator_snapshot_mailbox_replace(
            &mailbox, frame);
        if (sequence == 1)
            TEST_ASSERT_NULL(superseded);
        else
            TEST_ASSERT_EQUAL_PTR((void *)(sequence - 1), superseded);
    }
    TEST_ASSERT_EQUAL_UINT32(999, mailbox.replacement_count);
    TEST_ASSERT_EQUAL_PTR((void *)(uintptr_t)1000,
                          creator_snapshot_mailbox_take(&mailbox));
    TEST_ASSERT_NULL(creator_snapshot_mailbox_take(&mailbox));
}
