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
#define MUSIC_YIN_WINDOW_SIZE                   2048
#define MUSIC_FFT_SIZE                          4096
#define MUSIC_FFT_HOP_SIZE                      1024
#define MUSIC_MIN_FREQUENCY_HZ_INTEGER          65
#define MUSIC_MIN_FREQUENCY_HZ                  ((float)MUSIC_MIN_FREQUENCY_HZ_INTEGER)
#define MUSIC_MAX_FREQUENCY_HZ_INTEGER          2000
#define MUSIC_MAX_FREQUENCY_HZ                  ((float)MUSIC_MAX_FREQUENCY_HZ_INTEGER)
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
#define MUSIC_MELODY_HARMONIC_REL_TOLERANCE     0.04f
#define MUSIC_MELODY_YIN_FALLBACK_CONFIDENCE    0.65f
#define MUSIC_MELODY_FALLBACK_MIDI_MIN          48
#define MUSIC_MELODY_FALLBACK_MIDI_MAX          84
#define MUSIC_MELODY_MAX_SPECTRUM_CLASSES       4
#define MUSIC_SPECTRUM_SUPPORT_MIN_RELATIVE      0.35f
#define MUSIC_SPECTRUM_SUPPORT_MIN_PROMINENCE    1.10f
#define MUSIC_OCTAVE_CORRECTION_MIN_RELATIVE     0.45f
#define MUSIC_OCTAVE_CORRECTION_MIN_PROMINENCE   1.15f
#define MUSIC_ACTIVE_PITCH_CLASS_THRESHOLD      0.18f
#define MUSIC_CHORD_CONFIDENCE_THRESHOLD        0.66f
#define MUSIC_INTERVAL_CONFIDENCE_THRESHOLD     0.58f
#define MUSIC_INTERVAL_MAX_ACTIVE_CLASSES       4
#define MUSIC_FUNDAMENTAL_NOISE_MULTIPLIER      2.2f
#define MUSIC_FUNDAMENTAL_RELATIVE_THRESHOLD    0.04f
#define MUSIC_HARMONIC_SUPPORT_WEIGHT           0.45f
#define MUSIC_HARMONIC_SUPPRESSION_WEIGHT       0.85f
#define MUSIC_HARMONIC_RATIO_TOLERANCE          0.03f
#define MUSIC_HARMONIC_OWNER_MIN_RELATIVE       0.35f
#define MUSIC_NOISE_GATE_MULTIPLIER             2.5f
#define MUSIC_NOISE_FLOOR_RECOVERY_ALPHA        0.08f
#define MUSIC_MIN_RMS                           0.0008f
#define MUSIC_HIGH_PASS_HZ                      50.0f
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
#define MUSIC_ES7210_INPUT_GAIN_DB              27.0f
#define MUSIC_ES7210_GAIN_OPTION_1_DB           18.0f
#define MUSIC_ES7210_GAIN_OPTION_2_DB           21.0f
#define MUSIC_ES7210_GAIN_OPTION_3_DB           24.0f
#define MUSIC_ES7210_GAIN_OPTION_4_DB           27.0f
#define MUSIC_CLIP_NEAR_THRESHOLD               0.95f
#define MUSIC_CLIP_SAMPLE_THRESHOLD             0.98f
#define MUSIC_CLIP_RATE_THRESHOLD               0.001f
#define MUSIC_LOW_PEAK_GAIN_HINT                0.05f
#define MUSIC_CHORD_MIDI_MIN                    48
#define MUSIC_CHORD_MIDI_MAX                    84
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
_Static_assert(BOARD_MIC1_SLOT_INDEX < BOARD_AUDIO_CHANNELS, "MIC1 slot must fit the I2S frame");
#if !MUSIC_USE_SINGLE_MIC_CH1
_Static_assert(BOARD_MIC2_SLOT_INDEX < BOARD_AUDIO_CHANNELS, "MIC2 slot must fit the I2S frame");
#endif
