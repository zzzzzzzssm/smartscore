#include "dual_mic_selector.h"

#include <math.h>
#include <limits.h>
#include <string.h>

#include "music_detector_config.h"

static float clamp01(float value)
{
    return fmaxf(0.0f, fminf(1.0f, value));
}

static void evaluate_channel(dual_mic_quality_t *quality,
                             const audio_frame_metrics_t *metrics,
                             const audio_preprocess_state_t *preprocess)
{
    const float reference_noise = fmaxf(preprocess->noise_floor, MUSIC_MIN_RMS * 0.25f);
    const float ratio = metrics->rms / fmaxf(reference_noise, 1.0e-7f);
    quality->snr_db = ratio > 1.0e-6f ? 20.0f * log10f(ratio) : -120.0f;

    const bool zero_or_tiny = metrics->peak < MUSIC_MIC_MIN_VALID_PEAK;
    const bool dc_abnormal = fabsf(metrics->mean) > MUSIC_MIC_MAX_DC_OFFSET;
    const float crest_factor = metrics->rms > 1.0e-7f ? metrics->peak / metrics->rms : 0.0f;
    const bool impulsive = metrics->rms >= preprocess->noise_gate &&
                           crest_factor > MUSIC_MIC_MAX_CREST_FACTOR;
    const bool above_gate = preprocess->calibrated &&
                            metrics->rms >= preprocess->noise_gate;

    if (metrics->clipped) {
        quality->state = DUAL_MIC_CHANNEL_CLIPPED;
        quality->score = 0.0f;
        quality->signal_valid = false;
    } else if (dc_abnormal || impulsive) {
        quality->state = DUAL_MIC_CHANNEL_NOISY;
        quality->score = 0.05f;
        quality->signal_valid = false;
    } else if (!preprocess->calibrated || zero_or_tiny) {
        quality->state = DUAL_MIC_CHANNEL_INVALID;
        quality->score = 0.0f;
        quality->signal_valid = false;
    } else if (!above_gate) {
        quality->state = DUAL_MIC_CHANNEL_WEAK;
        quality->score = 0.10f * clamp01(metrics->rms / fmaxf(preprocess->noise_gate, 1.0e-7f));
        quality->signal_valid = false;
    } else {
        const float snr_score = clamp01((quality->snr_db - MUSIC_MIC_MIN_SNR_DB) /
                                        MUSIC_MIC_SNR_SCORE_RANGE_DB);
        const float headroom = clamp01((MUSIC_CLIP_NEAR_THRESHOLD - metrics->peak) /
                                       MUSIC_CLIP_NEAR_THRESHOLD);
        const float peak_score = clamp01((metrics->peak - MUSIC_MIC_MIN_VALID_PEAK) /
                                         MUSIC_MIC_PEAK_SCORE_RANGE);
        const float continuity = clamp01((float)quality->valid_streak /
                                         MUSIC_MIC_CONTINUITY_FRAMES);
        quality->score = clamp01(0.50f * snr_score + 0.20f * headroom +
                                 0.15f * peak_score + 0.15f * continuity);
        quality->signal_valid = quality->snr_db >= MUSIC_MIC_MIN_SNR_DB;
        quality->state = quality->signal_valid && quality->score >= MUSIC_MIC_GOOD_SCORE ?
            DUAL_MIC_CHANNEL_GOOD : DUAL_MIC_CHANNEL_VALID;
    }

    if (quality->signal_valid) {
        if (quality->valid_streak < UINT16_MAX) ++quality->valid_streak;
        quality->invalid_streak = 0;
    } else {
        if (quality->invalid_streak < UINT16_MAX) ++quality->invalid_streak;
        quality->valid_streak = 0;
    }
}

void dual_mic_selector_init(dual_mic_selector_t *selector)
{
    memset(selector, 0, sizeof(*selector));
    selector->selected_mic = 1;
    selector->health = DUAL_MIC_HEALTH_BOTH_INVALID;
}

void dual_mic_selector_update(dual_mic_selector_t *selector,
                              const audio_frame_metrics_t metrics[2],
                              const audio_preprocess_state_t preprocess[2],
                              dual_mic_selection_t *selection)
{
    for (int mic = 0; mic < 2; ++mic) {
        evaluate_channel(&selector->quality[mic], &metrics[mic], &preprocess[mic]);
    }

    const bool valid1 = selector->quality[0].signal_valid;
    const bool valid2 = selector->quality[1].signal_valid;
    if (valid1 && valid2) selector->health = DUAL_MIC_HEALTH_DUAL_OK;
    else if (valid1) selector->health = DUAL_MIC_HEALTH_MIC1_ONLY;
    else if (valid2) selector->health = DUAL_MIC_HEALTH_MIC2_ONLY;
    else selector->health = DUAL_MIC_HEALTH_BOTH_INVALID;

    const bool sound_active = valid1 || valid2;
    bool switched = false;
    if (sound_active) {
        selector->inactive_frames = 0;
        const int current = selector->selected_mic - 1;
        const bool current_valid = current == 0 ? valid1 : valid2;
        if (!current_valid) {
            const int replacement = valid1 ? 0 : 1;
            if (selector->selected_mic != replacement + 1) {
                selector->selected_mic = replacement + 1;
                switched = true;
            }
            selector->note_locked = true;
            selector->challenger_frames = 0;
            selector->hold_frames = 0;
        } else if (!selector->note_locked) {
            int preferred = selector->selected_mic - 1;
            if (valid1 && !valid2) {
                preferred = 0;
            } else if (valid2 && !valid1) {
                preferred = 1;
            } else if (valid1 && valid2 &&
                       selector->quality[1].score > selector->quality[0].score) {
                preferred = 1;
            }
            if (selector->selected_mic != preferred + 1) {
                selector->selected_mic = preferred + 1;
                switched = true;
            }
            selector->note_locked = true;
            selector->challenger_frames = 0;
            selector->hold_frames = 0;
        }
    } else {
        selector->challenger_frames = 0;
        if (selector->inactive_frames < UINT_MAX) ++selector->inactive_frames;
        if (selector->inactive_frames >= MUSIC_MIC_NOTE_RELEASE_FRAMES) {
            selector->note_locked = false;
        }
    }
    if (!switched && selector->hold_frames < UINT_MAX) ++selector->hold_frames;
    if (switched) ++selector->switch_count;

    selection->selected_mic = selector->selected_mic;
    selection->sound_active = sound_active;
    selection->switched = switched;
    selection->health = selector->health;
}

const char *dual_mic_channel_state_name(dual_mic_channel_state_t state)
{
    switch (state) {
        case DUAL_MIC_CHANNEL_WEAK: return "WEAK";
        case DUAL_MIC_CHANNEL_VALID: return "VALID";
        case DUAL_MIC_CHANNEL_GOOD: return "GOOD";
        case DUAL_MIC_CHANNEL_CLIPPED: return "CLIPPED";
        case DUAL_MIC_CHANNEL_NOISY: return "NOISY";
        case DUAL_MIC_CHANNEL_INVALID:
        default: return "INVALID";
    }
}

const char *dual_mic_health_name(dual_mic_health_t health)
{
    switch (health) {
        case DUAL_MIC_HEALTH_MIC1_ONLY: return "MIC1_ONLY";
        case DUAL_MIC_HEALTH_MIC2_ONLY: return "MIC2_ONLY";
        case DUAL_MIC_HEALTH_DUAL_OK: return "DUAL_OK";
        case DUAL_MIC_HEALTH_BOTH_INVALID:
        default: return "BOTH_INVALID";
    }
}
