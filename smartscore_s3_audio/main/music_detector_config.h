#pragma once

#include "board_pins.h"

#ifndef MUSIC_USE_SINGLE_MIC_CH1
#define MUSIC_USE_SINGLE_MIC_CH1                0
#endif

#if MUSIC_USE_SINGLE_MIC_CH1 != 0 && MUSIC_USE_SINGLE_MIC_CH1 != 1
#error "MUSIC_USE_SINGLE_MIC_CH1 must be 0 or 1"
#endif

typedef enum {
    MUSIC_MIC_MODE_SINGLE_CH1 = 0,
    MUSIC_MIC_MODE_DUAL,
} music_mic_mode_t;

#define MUSIC_DEFAULT_MIC_MODE                  \
    (MUSIC_USE_SINGLE_MIC_CH1 ? MUSIC_MIC_MODE_SINGLE_CH1 : MUSIC_MIC_MODE_DUAL)

#define MUSIC_SAMPLE_RATE_HZ                    BOARD_AUDIO_SAMPLE_RATE_HZ
#define MUSIC_REFERENCE_A4_HZ                   440.0f
#define MUSIC_CAPTURE_FRAMES                    1024
#define MUSIC_CAPTURE_BUFFER_COUNT              3
#define MUSIC_CAPTURE_TASK_STACK_SIZE           6144
#define MUSIC_CAPTURE_STACK_WARN_BYTES           512U
#define MUSIC_CAPTURE_STACK_CHECK_INTERVAL_MS   5000U
#define MUSIC_DSP_TASK_STACK_SIZE               12288
#define MUSIC_DSP_STACK_WARN_BYTES               1024U
#define MUSIC_DSP_TASK_PRIORITY                    12
#define MUSIC_DSP_TASK_CORE                         1
#define MUSIC_YIN_WINDOW_SIZE                   2048
#define MUSIC_LOW_YIN_SAMPLE_RATE_HZ            6000
#define MUSIC_LOW_YIN_WINDOW_SIZE               1024
#define MUSIC_LOW_YIN_DECIMATION                (MUSIC_SAMPLE_RATE_HZ / MUSIC_LOW_YIN_SAMPLE_RATE_HZ)
#define MUSIC_LOW_YIN_HISTORY_FRAMES               3U
#define MUSIC_LOW_YIN_STABLE_VOTES                 2U
#define MUSIC_LOW_MATCH_RING_SIZE                2048U
#define MUSIC_LOW_MATCH_WINDOW_SIZE              1536U
#define MUSIC_LOW_MATCH_FIR_TAPS                   63U
#define MUSIC_LOW_MATCH_CUTOFF_HZ               2200.0f
#define MUSIC_LOW_MATCH_MAX_HARMONIC_HZ         2000.0f
#define MUSIC_LOW_MATCH_MAX_HARMONICS               8U
#define MUSIC_LOW_MATCH_MAX_KEYS                    4U
#define MUSIC_LOW_MATCH_PRIMARY_CONFIDENCE        0.82f
#define MUSIC_LOW_MATCH_SECONDARY_CONFIDENCE      0.78f
#define MUSIC_LOW_MATCH_STRONG_CONFIDENCE         0.92f
#define MUSIC_LOW_MATCH_SECONDARY_RATIO           0.52f
#define MUSIC_LOW_MATCH_RELEASE_FRAMES               2U
#define MUSIC_LOW_MATCH_ATTACK_FRAMES                2U
#define MUSIC_LOW_MATCH_LEAKAGE_SUBTRACT          1.00f
#define MUSIC_LOW_MATCH_MIN_AMPLITUDE             0.00020f
#define MUSIC_FFT_SIZE                          4096
#define MUSIC_FFT_HOP_SIZE                      1024
#define MUSIC_MIN_FREQUENCY_HZ_INTEGER          65
#define MUSIC_MIN_FREQUENCY_HZ                  ((float)MUSIC_MIN_FREQUENCY_HZ_INTEGER)
#define MUSIC_MAX_FREQUENCY_HZ_INTEGER          2200
#define MUSIC_MAX_FREQUENCY_HZ                  ((float)MUSIC_MAX_FREQUENCY_HZ_INTEGER)
/* The dedicated low-note path covers C2..B3. The high-rate YIN starts just
 * below C4 so common C3/F3 periods from upper chords cannot take its vote. */
#define MUSIC_LOW_MIN_FREQUENCY_HZ               55.0f
#define MUSIC_HIGH_YIN_MIN_FREQUENCY_HZ         250.0f
#define MUSIC_LOW_YIN_MAX_FREQUENCY_HZ          255.0f
#define MUSIC_VIRTUAL_FUNDAMENTAL_MAX_MIDI       59
#define MUSIC_LOW_CHORD_YIN_CONFIDENCE           0.85f
#define MUSIC_SPECTRUM_MAX_FREQUENCY_HZ         10000.0f
#define MUSIC_YIN_THRESHOLD                     0.20f
#define MUSIC_TUNING_TRACK_MIN_CONFIDENCE       0.75f
#define MUSIC_TUNING_TRACK_MAX_SAMPLE_CENTS     40.0f
#define MUSIC_TUNING_TRACK_MAX_OFFSET_CENTS     35.0f
#define MUSIC_TUNING_TRACK_ALPHA                0.03f
#define MUSIC_YIN_CONFIDENCE_THRESHOLD          0.70f
#define MUSIC_SINGLE_HARMONIC_RATIO_THRESHOLD   0.74f
#define MUSIC_SINGLE_DOMINANCE_YIN_CONFIDENCE   0.90f
#define MUSIC_SINGLE_DOMINANCE_HARMONIC_RATIO   0.90f
#define MUSIC_YIN_SPECTRAL_SINGLE_CONFIDENCE    0.88f
#define MUSIC_MELODY_YIN_OVERRIDE_CONFIDENCE    0.80f
#define MUSIC_MELODY_STRONG_YIN_CONFIDENCE      0.90f
#define MUSIC_MELODY_HARMONIC_REL_TOLERANCE     0.04f
#define MUSIC_MELODY_YIN_FALLBACK_CONFIDENCE    0.65f
#define MUSIC_MELODY_FALLBACK_MIDI_MIN          48
#define MUSIC_MELODY_FALLBACK_MIDI_MAX          84
#define MUSIC_MELODY_MAX_SPECTRUM_CLASSES       4
#define MUSIC_SPECTRUM_SUPPORT_MIN_RELATIVE      0.35f
#define MUSIC_SPECTRUM_SUPPORT_MIN_PROMINENCE    1.10f
#define MUSIC_OCTAVE_CORRECTION_MIN_RELATIVE     0.45f
#define MUSIC_OCTAVE_CORRECTION_MIN_PROMINENCE   1.15f
#define MUSIC_OCTAVE_CONTINUITY_MIN_RELATIVE     0.25f
#define MUSIC_OCTAVE_CONTINUITY_MIN_PROMINENCE   1.05f
#define MUSIC_VIRTUAL_ROOT_QUARANTINE_MS          500U
#define MUSIC_VIRTUAL_ROOT_HOLD_MIN_RELATIVE      0.18f
#define MUSIC_VIRTUAL_ROOT_HOLD_MIN_PROMINENCE    1.02f
#define MUSIC_ACTIVE_PITCH_CLASS_THRESHOLD      0.18f
#define MUSIC_CHORD_CONFIDENCE_THRESHOLD        0.66f
#define MUSIC_INTERVAL_CONFIDENCE_THRESHOLD     0.58f
#define MUSIC_INTERVAL_MAX_ACTIVE_CLASSES       4
#define MUSIC_FUNDAMENTAL_NOISE_MULTIPLIER      2.2f
#define MUSIC_FUNDAMENTAL_RELATIVE_THRESHOLD    0.04f

/* The demo profile is enabled only by the P4 note-monitor session.  Absolute
 * noise rejection and temporal voting stay unchanged, while weak independent
 * fundamentals are allowed to contribute to an interval/chord. */
#define MUSIC_DEMO_INTERVAL_MAX_ACTIVE_CLASSES   5
#define MUSIC_DEMO_INITIAL_GAIN_DB                9.0f
#define MUSIC_DEMO_MIN_GAIN_DB                    3.0f
#define MUSIC_DEMO_MAX_GAIN_DB                   30.0f
#define MUSIC_DEMO_GAIN_STEP_DB                   3.0f
#define MUSIC_DEMO_CLIP_GAIN_STEP_DB              6.0f
#define MUSIC_DEMO_GAIN_SETTLE_MS               350U
#define MUSIC_DEMO_CLIP_SETTLE_MS                 80U
#define MUSIC_DEMO_CLIP_CONFIRM_FRAMES            1U
#define MUSIC_DEMO_LOW_PEAK_THRESHOLD            0.08f
#define MUSIC_DEMO_LOW_PEAK_RAISE_MS           3000U
#define MUSIC_DEMO_AMBIENT_TRACK_ALPHA            0.006f
#define MUSIC_DEMO_SNR_HIGH_DB                   18.0f
#define MUSIC_DEMO_SNR_MEDIUM_DB                 10.0f
#define MUSIC_DEMO_SNR_MIN_DB                     6.0f
#define MUSIC_DEMO_HIGH_SNR_INTERVAL_THRESHOLD    0.48f
#define MUSIC_DEMO_MEDIUM_SNR_INTERVAL_THRESHOLD  0.52f
#define MUSIC_DEMO_HIGH_SNR_CHORD_THRESHOLD       0.58f
#define MUSIC_DEMO_MEDIUM_SNR_CHORD_THRESHOLD     0.61f
#define MUSIC_DEMO_HIGH_SNR_ACTIVE_THRESHOLD      0.12f
#define MUSIC_DEMO_MEDIUM_SNR_ACTIVE_THRESHOLD    0.15f
#define MUSIC_DEMO_HIGH_SNR_RELATIVE_THRESHOLD    0.020f
#define MUSIC_DEMO_MEDIUM_SNR_RELATIVE_THRESHOLD  0.030f
#define MUSIC_DEMO_HIGH_SNR_INTERVAL_VOTES         2
#define MUSIC_DEMO_MEDIUM_SNR_INTERVAL_VOTES       3
#define MUSIC_DEMO_CHORD_STABLE_VOTES              3
#define MUSIC_DEMO_POLY_MAX_ACTIVE_CLASSES         4
#define MUSIC_DEMO_POLY_MIN_RELATIVE              0.26f
#define MUSIC_DEMO_POLY_MIN_PROMINENCE            1.04f
#define MUSIC_DEMO_CHORD_MIN_RELATIVE             0.28f
#define MUSIC_DEMO_CHORD_MIN_PROMINENCE           1.06f
#define MUSIC_DEMO_STRONG_SINGLE_SECONDARY_RELATIVE 0.38f
#define MUSIC_DEMO_ADJACENT_MIN_RELATIVE          0.45f
#define MUSIC_DEMO_ADJACENT_MIN_PROMINENCE        1.25f
#define MUSIC_DEMO_ADJACENT_MIN_PEAK_BINS          3
#define MUSIC_FFT_POLY_PRIORITY_MIN_MIDI            60
#define MUSIC_HIGH_FFT_MIN_RELATIVE                0.24f
#define MUSIC_HIGH_FFT_MIN_PROMINENCE              1.04f
#define MUSIC_HIGH_FFT_FAST_MIN_RELATIVE           0.32f
#define MUSIC_HIGH_FFT_FAST_MIN_PROMINENCE         1.08f
#define MUSIC_HIGH_FFT_FAST_INTERVAL_CONFIDENCE    0.78f
#define MUSIC_HIGH_FFT_FAST_CHORD_CONFIDENCE       0.86f
#define MUSIC_LOW_POLY_YIN_CONFIDENCE              0.68f
#define MUSIC_SPECTRUM_LOW_BAND_MAX_HZ           250.0f
#define MUSIC_SPECTRUM_MID_BAND_MAX_HZ           800.0f
#define MUSIC_HARMONIC_SUPPORT_WEIGHT           0.45f
#define MUSIC_HARMONIC_SUPPRESSION_WEIGHT       0.85f
#define MUSIC_HARMONIC_RATIO_TOLERANCE          0.03f
#define MUSIC_HARMONIC_OWNER_MIN_RELATIVE       0.35f
#define MUSIC_NOISE_GATE_MULTIPLIER             2.5f
#define MUSIC_NOISE_FLOOR_RECOVERY_ALPHA        0.08f
#define MUSIC_MIN_RMS                           0.0008f
#define MUSIC_HIGH_PASS_HZ                      25.0f
#define MUSIC_CALIBRATION_MS                    1000
#define MUSIC_STABLE_HISTORY_SIZE               5
#define MUSIC_STABLE_VOTE_COUNT                 3
#define MUSIC_ACTIVITY_HANGOVER_BLOCKS           4
#define MUSIC_SPECTRUM_ANALYSIS_HOPS             2U
#define MUSIC_ONSET_RISE_RATIO                   1.45f
#define MUSIC_ONSET_MIN_RMS_RISE                 0.0006f
#define MUSIC_ONSET_MIN_INTERVAL_MS              180U
#define MUSIC_ONSET_ASSOCIATION_MS               320U
#define MUSIC_UART_SAME_NOTE_RETRIGGER_MS        180U
#define MUSIC_RESULT_REPEAT_INTERVAL_MS         300
#define MUSIC_RESULT_LOG_REPEAT_INTERVAL_MS     1500
#define MUSIC_DIAGNOSTIC_INTERVAL_MS            1500
#define MUSIC_PERFORMANCE_INTERVAL_MS           2000
#define MUSIC_ES7210_INPUT_GAIN_DB              30.0f
#define MUSIC_ES7210_GAIN_OPTION_LOW_1_DB        3.0f
#define MUSIC_ES7210_GAIN_OPTION_LOW_2_DB        6.0f
#define MUSIC_ES7210_GAIN_OPTION_1_DB            9.0f
#define MUSIC_ES7210_GAIN_OPTION_2_DB           12.0f
#define MUSIC_ES7210_GAIN_OPTION_3_DB           15.0f
#define MUSIC_ES7210_GAIN_OPTION_4_DB           18.0f
#define MUSIC_ES7210_GAIN_OPTION_5_DB           21.0f
#define MUSIC_ES7210_GAIN_OPTION_6_DB           24.0f
#define MUSIC_ES7210_GAIN_OPTION_7_DB           27.0f
#define MUSIC_ES7210_GAIN_OPTION_8_DB           30.0f
#define MUSIC_CLIP_NEAR_THRESHOLD               0.95f
#define MUSIC_CLIP_SAMPLE_THRESHOLD             0.98f
#define MUSIC_CLIP_RATE_THRESHOLD               0.001f
#define MUSIC_LOW_PEAK_GAIN_HINT                0.05f
/* Full 61-key keyboard range for the exact-key evidence layer and the note
 * tracker. The legacy chord/mid-high classification keeps its 48..84 range. */
#define MUSIC_PIANO_MIDI_MIN                    36
#define MUSIC_PIANO_MIDI_MAX                    96
#define MUSIC_PIANO_KEY_COUNT                   (MUSIC_PIANO_MIDI_MAX - MUSIC_PIANO_MIDI_MIN + 1)
#define MUSIC_MAX_SIMULTANEOUS_KEYS             4
#define MUSIC_CHORD_MIDI_MIN                    48
#define MUSIC_CHORD_MIDI_MAX                    MUSIC_PIANO_MIDI_MAX
#define MUSIC_EXACT_KEY_SALIENCE_FLOOR          0.16f
#define MUSIC_MIC_SWITCH_CONFIRM_FRAMES         3
#define MUSIC_MIC_SWITCH_SCORE_MARGIN           0.15f
#define MUSIC_MIC_MIN_HOLD_FRAMES               8U
#define MUSIC_MIC_NOTE_RELEASE_FRAMES           2U
#define MUSIC_MIC_MIN_VALID_PEAK                0.0012f
#define MUSIC_MIC_MAX_DC_OFFSET                 0.08f
#define MUSIC_MIC_MAX_CREST_FACTOR              10.0f
#define MUSIC_MIC_MIN_SNR_DB                    6.0f
#define MUSIC_MIC_SNR_SCORE_RANGE_DB            24.0f
#define MUSIC_MIC_PEAK_SCORE_RANGE              0.20f
#define MUSIC_MIC_CONTINUITY_FRAMES              4.0f
#define MUSIC_MIC_GOOD_SCORE                    0.68f

/* Dedicated NDJSON music link. State events have a separate, larger queue so
 * continuous pitch traffic cannot displace note transitions. */
#define MUSIC_UART_STATE_QUEUE_LENGTH           16
#define MUSIC_UART_PITCH_QUEUE_LENGTH           8
#define MUSIC_UART_RX_BUFFER_SIZE               1024
#define MUSIC_UART_TX_BUFFER_SIZE               1024
#define MUSIC_UART_TASK_STACK_SIZE              4096
#define MUSIC_UART_TASK_PRIORITY                6
#define MUSIC_UART_TASK_CORE                    0
#define MUSIC_UART_HEARTBEAT_INTERVAL_MS        1000
#define MUSIC_UART_UNKNOWN_TIMEOUT_MS           300
#define MUSIC_UART_PITCH_INTERVAL_MS            50
#define MUSIC_UART_VELOCITY_FULL_SCALE_RMS      0.05f

_Static_assert(MUSIC_FFT_SIZE >= MUSIC_YIN_WINDOW_SIZE, "FFT window must contain YIN window");
_Static_assert(MUSIC_REFERENCE_A4_HZ >= 400.0f && MUSIC_REFERENCE_A4_HZ <= 480.0f,
               "reference tuning must be a plausible A4 frequency");
_Static_assert((MUSIC_FFT_SIZE % MUSIC_FFT_HOP_SIZE) == 0, "FFT hop must divide FFT size");
_Static_assert(MUSIC_SPECTRUM_ANALYSIS_HOPS > 0,
               "spectrum analysis hop interval must be positive");
_Static_assert(MUSIC_CAPTURE_FRAMES == MUSIC_FFT_HOP_SIZE,
               "capture blocks must map directly onto FFT hop segments");
_Static_assert(MUSIC_LOW_YIN_DECIMATION == 4,
               "low-note YIN path requires exact 24 kHz to 6 kHz decimation");
_Static_assert(MUSIC_LOW_MATCH_RING_SIZE >= MUSIC_LOW_MATCH_WINDOW_SIZE,
               "low-note matching ring must contain its analysis window");
_Static_assert((MUSIC_LOW_MATCH_RING_SIZE & (MUSIC_LOW_MATCH_RING_SIZE - 1U)) == 0,
               "low-note matching ring must be a power of two");
_Static_assert((MUSIC_LOW_MATCH_FIR_TAPS & 1U) == 1U,
               "low-note decimator requires an odd FIR length");
_Static_assert(MUSIC_PIANO_KEY_COUNT == 61, "configured keyboard must contain 61 keys");
_Static_assert(BOARD_MIC1_SLOT_INDEX < BOARD_AUDIO_CHANNELS, "MIC1 slot must fit the I2S frame");
#if !MUSIC_USE_SINGLE_MIC_CH1
_Static_assert(BOARD_MIC2_SLOT_INDEX < BOARD_AUDIO_CHANNELS, "MIC2 slot must fit the I2S frame");
#endif
