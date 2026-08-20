#include "adaptive_input_control.h"

#include <math.h>
#include <string.h>

#include "music_detector_config.h"

void adaptive_input_control_init(adaptive_input_control_t *control,
                                 float current_gain_db)
{
    memset(control, 0, sizeof(*control));
    control->current_gain_db = current_gain_db;
}

bool adaptive_input_control_profile_target(
    const adaptive_input_control_t *control, bool demo_profile,
    float *requested_gain_db, adaptive_gain_reason_t *reason)
{
    const float target = demo_profile ? MUSIC_DEMO_INITIAL_GAIN_DB
                                      : MUSIC_ES7210_INPUT_GAIN_DB;
    if (fabsf(control->current_gain_db - target) < 0.1f) return false;
    *requested_gain_db = target;
    *reason = demo_profile ? ADAPTIVE_GAIN_REASON_DEMO_ENTER
                           : ADAPTIVE_GAIN_REASON_STRICT_RESTORE;
    return true;
}

bool adaptive_input_control_update(
    adaptive_input_control_t *control, const audio_frame_metrics_t *metrics,
    size_t metric_count, int selected_mic, float selected_snr_db,
    uint32_t now_ms, float *requested_gain_db,
    adaptive_gain_reason_t *reason)
{
    if (control == NULL || metrics == NULL || metric_count == 0 ||
        selected_mic < 1 || (size_t)selected_mic > metric_count) {
        return false;
    }
    const audio_frame_metrics_t *selected = &metrics[selected_mic - 1];
    if (selected->clipped) {
        ++control->clip_streak;
        control->low_peak_since_ms = 0;
    } else {
        control->clip_streak = 0;
    }

    const bool clip_settled = control->last_change_ms == 0 ||
        now_ms - control->last_change_ms >= MUSIC_DEMO_CLIP_SETTLE_MS;
    if (clip_settled &&
        control->clip_streak >= MUSIC_DEMO_CLIP_CONFIRM_FRAMES &&
        control->current_gain_db > MUSIC_DEMO_MIN_GAIN_DB + 0.1f) {
        *requested_gain_db = fmaxf(MUSIC_DEMO_MIN_GAIN_DB,
                                   control->current_gain_db -
                                       MUSIC_DEMO_CLIP_GAIN_STEP_DB);
        *reason = ADAPTIVE_GAIN_REASON_CLIPPING;
        return true;
    }

    const bool settled = control->last_change_ms == 0 ||
                         now_ms - control->last_change_ms >=
                             MUSIC_DEMO_GAIN_SETTLE_MS;

    const bool low_clean_signal = !selected->clipped &&
        selected->peak > MUSIC_MIC_MIN_VALID_PEAK &&
        selected->peak < MUSIC_DEMO_LOW_PEAK_THRESHOLD &&
        selected_snr_db >= MUSIC_DEMO_SNR_MIN_DB;
    if (low_clean_signal) {
        if (control->low_peak_since_ms == 0) {
            control->low_peak_since_ms = now_ms;
        }
    } else {
        control->low_peak_since_ms = 0;
    }
    if (settled && control->low_peak_since_ms != 0 &&
        now_ms - control->low_peak_since_ms >=
            MUSIC_DEMO_LOW_PEAK_RAISE_MS &&
        control->current_gain_db < MUSIC_DEMO_MAX_GAIN_DB - 0.1f) {
        *requested_gain_db = fminf(MUSIC_DEMO_MAX_GAIN_DB,
                                   control->current_gain_db +
                                       MUSIC_DEMO_GAIN_STEP_DB);
        *reason = ADAPTIVE_GAIN_REASON_LOW_SIGNAL;
        return true;
    }
    return false;
}

void adaptive_input_control_applied(adaptive_input_control_t *control,
                                    float gain_db, uint32_t now_ms)
{
    control->current_gain_db = gain_db;
    control->clip_streak = 0;
    control->low_peak_since_ms = 0;
    control->last_change_ms = now_ms;
}

const char *adaptive_gain_reason_name(adaptive_gain_reason_t reason)
{
    switch (reason) {
        case ADAPTIVE_GAIN_REASON_DEMO_ENTER: return "demo-enter";
        case ADAPTIVE_GAIN_REASON_STRICT_RESTORE: return "strict-restore";
        case ADAPTIVE_GAIN_REASON_CLIPPING: return "clipping";
        case ADAPTIVE_GAIN_REASON_LOW_SIGNAL: return "low-signal";
        case ADAPTIVE_GAIN_REASON_NONE:
        default: return "none";
    }
}
