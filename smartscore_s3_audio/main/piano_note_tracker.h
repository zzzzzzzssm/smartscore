#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "chord_detector.h"
#include "low_note_detector.h"
#include "yin_detector.h"

#define PIANO_NOTE_SET_MAX_KEYS 4
#define PIANO_TRACKER_MIDI_MIN 36
#define PIANO_TRACKER_MIDI_MAX 96
#define PIANO_TRACKER_ATTACK_THRESHOLD 0.45f
#define PIANO_TRACKER_RELEASE_THRESHOLD 0.16f
#define PIANO_TRACKER_ATTACK_FRAMES 2U
#define PIANO_TRACKER_RELEASE_FRAMES 2U
#define PIANO_TRACKER_LOW_YIN_CONFIDENCE 0.68f
#define PIANO_TRACKER_HIGH_YIN_CONFIDENCE 0.72f
#define PIANO_TRACKER_SINGLE_LOCK_YIN_CONFIDENCE 0.85f
#define PIANO_TRACKER_SINGLE_LOCK_HARMONIC_RATIO 0.85f
#define PIANO_TRACKER_SINGLE_MIN_CONFIDENCE 0.85f
#define PIANO_TRACKER_SINGLE_HIGH_HARMONIC_RATIO 0.70f
#define PIANO_TRACKER_SINGLE_LOW_HARMONIC_RATIO 0.45f
#define PIANO_TRACKER_SINGLE_C4_HARMONIC_RATIO 0.30f
#define PIANO_TRACKER_SINGLE_SALIENCE 0.25f
#define PIANO_TRACKER_SINGLE_PROMINENCE 1.02f
#define PIANO_TRACKER_INDEPENDENT_SALIENCE 0.50f
#define PIANO_TRACKER_INDEPENDENT_PROMINENCE 1.20f
#define PIANO_TRACKER_SECONDARY_SALIENCE 0.18f
#define PIANO_TRACKER_SECONDARY_PROMINENCE 1.05f
#define PIANO_TRACKER_SECONDARY_CONFIRM_FRAMES 3U
#define PIANO_TRACKER_POLY_STABLE_FRAMES 2U
#define PIANO_TRACKER_CHORD_STABLE_FRAMES 3U
#define PIANO_TRACKER_POLY_CANDIDATE_MISS_FRAMES 1U
#define PIANO_TRACKER_POLY_RELEASE_FRAMES 2U
#define PIANO_TRACKER_POLY_INTERVAL_CONFIDENCE 0.62f
#define PIANO_TRACKER_POLY_CHORD_CONFIDENCE 0.68f
#define PIANO_TRACKER_POLY_SALIENCE 0.25f
#define PIANO_TRACKER_POLY_PROMINENCE 1.02f
#define PIANO_TRACKER_POLY_HARMONIC_SOURCE_SALIENCE 0.30f
#define PIANO_TRACKER_POLY_HARMONIC_SOURCE_PROMINENCE 0.98f
#define PIANO_TRACKER_POLY_HARMONIC_MAX 12
#define PIANO_TRACKER_POLY_HARMONIC_MAX_CENTS 80.0f
#define PIANO_TRACKER_INTERVAL_ALIGN_MAX_CENTS 65.0f
#define PIANO_TRACKER_ONSET_RISE_RATIO 1.30f
#define PIANO_TRACKER_ONSET_MIN_RISE 0.07f
#define PIANO_TRACKER_CO_ONSET_MS 150U
#define PIANO_TRACKER_CANDIDATE_TIMEOUT_MS 260U
#define PIANO_TRACKER_CLIP_HOLD_MS 260U
#define PIANO_TRACKER_LOW_TAIL_MIDI_MAX 59
#define PIANO_TRACKER_LOW_TAIL_PEAK_RATIO 0.55f
#define PIANO_TRACKER_LOW_TAIL_RELEASE_RATIO 0.35f
#define PIANO_TRACKER_LOW_TAIL_FALL_FRAMES 2U
#define PIANO_TRACKER_LOW_YIN_SUSTAIN_CONFIDENCE 0.75f
#define PIANO_TRACKER_LOW_REARM_QUIET_FRAMES 2U
#define PIANO_TRACKER_LOW_VIRTUAL_SALIENCE 0.85f
#define PIANO_TRACKER_LOW_VIRTUAL_PROMINENCE 2.50f
#define PIANO_TRACKER_VELOCITY_RMS 0.05f

typedef struct {
    uint32_t timestamp_ms;
    uint8_t count;
    int midi[PIANO_NOTE_SET_MAX_KEYS];
    uint8_t velocity[PIANO_NOTE_SET_MAX_KEYS];
    float confidence[PIANO_NOTE_SET_MAX_KEYS];
    bool overflow;
    bool degraded_mic;
} piano_note_set_t;

typedef struct {
    /* Reused analysis scratch. The owning detector context is allocated in
     * PSRAM, so exact-key updates do not create a 61-float task-stack array. */
    float evidence[CHORD_PIANO_KEY_COUNT];
    bool active[CHORD_PIANO_KEY_COUNT];
    uint8_t attack_frames[CHORD_PIANO_KEY_COUNT];
    uint8_t release_frames[CHORD_PIANO_KEY_COUNT];
    float onset_confidence[CHORD_PIANO_KEY_COUNT];
    uint8_t onset_velocity[CHORD_PIANO_KEY_COUNT];
    /* A weaker second physical fundamental may be stable across time even
     * when it misses the strict single-frame polyphony threshold. Each slot
     * is anchored to the YIN-primary key against which it accumulated. */
    uint8_t secondary_frames[CHORD_PIANO_KEY_COUNT];
    int8_t secondary_anchor_key[CHORD_PIANO_KEY_COUNT];
    bool secondary_confirmed[CHORD_PIANO_KEY_COUNT];
    float previous_evidence[CHORD_PIANO_KEY_COUNT];
    float peak_evidence[CHORD_PIANO_KEY_COUNT];
    uint32_t candidate_onset_ms[CHORD_PIANO_KEY_COUNT];
    uint32_t onset_ms[CHORD_PIANO_KEY_COUNT];
    uint8_t falling_frames[CHORD_PIANO_KEY_COUNT];
    bool fresh_onset[CHORD_PIANO_KEY_COUNT];
    bool decay_tail[CHORD_PIANO_KEY_COUNT];
    bool low_rearm_blocked[CHORD_PIANO_KEY_COUNT];
    uint8_t low_rearm_quiet_frames[CHORD_PIANO_KEY_COUNT];
    int poly_candidate_midi[PIANO_NOTE_SET_MAX_KEYS];
    int poly_locked_midi[PIANO_NOTE_SET_MAX_KEYS];
    uint8_t poly_candidate_count;
    uint8_t poly_candidate_frames;
    uint8_t poly_candidate_missing_frames;
    uint8_t poly_locked_count;
    uint8_t poly_missing_frames;
    bool poly_lock_active;
    bool single_lock_active;
    int single_lock_midi;
    bool interval_alignment_active;
    int interval_alignment_from_midi;
    int interval_alignment_to_midi;
    float previous_rms;
    uint32_t last_clean_ms;
    bool degraded_mic;
    bool degraded_initialized;
} piano_note_tracker_t;

void piano_note_tracker_init(piano_note_tracker_t *tracker);

/* Updates a debounced exact-key state. Returns true only when the active MIDI
 * set changed; out_set is populated on every call. */
bool piano_note_tracker_update(piano_note_tracker_t *tracker,
                               const chord_result_t *spectrum,
                               const yin_result_t *high_yin,
                               const yin_result_t *low_yin,
                               const low_note_result_t *low_notes,
                               float harmonic_explained_ratio,
                               bool signal_active, bool allow_attack,
                               bool degraded_mic, float rms,
                               uint32_t timestamp_ms,
                               piano_note_set_t *out_set);

/* Releases all keys, returning true when a non-empty set was cleared. */
bool piano_note_tracker_release_all(piano_note_tracker_t *tracker,
                                    uint32_t timestamp_ms,
                                    piano_note_set_t *out_set);
