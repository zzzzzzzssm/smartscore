#include "diagnostics.h"

#include <inttypes.h>
#include "esp_log.h"
#include "music_detector_config.h"

static diagnostics_counters_t s_counters;

diagnostics_counters_t *diagnostics_counters(void)
{
    return &s_counters;
}

void diagnostics_log_audio(float mic1_rms, float mic1_peak, float mic1_clip,
                           float mic2_rms, float mic2_peak, float mic2_clip,
                           int selected_mic, float mic1_noise, float mic2_noise,
                           float mic1_gate, float mic2_gate)
{
#if MUSIC_USE_SINGLE_MIC_CH1
    (void)mic2_rms;
    (void)mic2_peak;
    (void)mic2_clip;
    (void)mic2_noise;
    (void)mic2_gate;
    ESP_LOGI("AUDIO_DIAG", "ch1_rms=%.5f peak=%.3f clip=%.4f gate=%.5f noise=%.5f",
             mic1_rms, mic1_peak, mic1_clip, mic1_gate, mic1_noise);
    ESP_LOGI("AUDIO_DIAG", "selected_mic=%d queue_overflow=%" PRIu32 " i2s_errors=%" PRIu32,
             selected_mic, s_counters.queue_overflow_count, s_counters.i2s_read_error_count);
#else
    ESP_LOGI("AUDIO_DIAG", "MIC1 rms=%.5f peak=%.3f clip=%.4f MIC2 rms=%.5f peak=%.3f clip=%.4f",
             mic1_rms, mic1_peak, mic1_clip, mic2_rms, mic2_peak, mic2_clip);
    ESP_LOGI("AUDIO_DIAG", "selected_mic=%d noise=[%.5f,%.5f] gate=[%.5f,%.5f] queue_overflow=%" PRIu32
             " i2s_errors=%" PRIu32, selected_mic, mic1_noise, mic2_noise, mic1_gate, mic2_gate,
             s_counters.queue_overflow_count, s_counters.i2s_read_error_count);
#endif
}

void diagnostics_log_performance(unsigned queue_depth, unsigned free_heap,
                                 unsigned stack_words)
{
    const uint32_t average_us = s_counters.dsp_cycle_count > 0 ?
        (uint32_t)(s_counters.dsp_cycle_sum_us / s_counters.dsp_cycle_count) : 0;
#if MUSIC_USE_SINGLE_MIC_CH1
    ESP_LOGI("DSP_PERF", "yin=%" PRIu32 "us fft=%" PRIu32 "us chord=%" PRIu32
             "us low_yin=%" PRIu32 "us low_fft=%" PRIu32
             "us total=%" PRIu32 "us queue=%u dropped=%" PRIu32
             " diag_drop=%" PRIu32 " deadline_miss=%" PRIu32
             " avg=%" PRIu32 "us max=%" PRIu32
             "us exhausted=%" PRIu32 " heap=%u stack_min=%u words",
             s_counters.yin_time_us, s_counters.mic1_fft_time_us,
             s_counters.chord_time_us, s_counters.low_yin_time_us,
             s_counters.low_spectrum_time_us, s_counters.dsp_cycle_time_us,
             queue_depth,
             s_counters.dropped_buffer_count,
             s_counters.diagnostic_drop_count,
             s_counters.dsp_deadline_miss_count,
             average_us, s_counters.dsp_cycle_max_us, s_counters.buffer_exhaustion_count,
             free_heap, stack_words);
#else
    ESP_LOGI("DSP_PERF", "yin=%" PRIu32 "us fft=%" PRIu32
             "us chord=%" PRIu32 "us low_yin=%" PRIu32
             "us low_fft=%" PRIu32 "us total=%" PRIu32 "us avg=%" PRIu32
             "us max=%" PRIu32 "us queue=%u dropped=%" PRIu32
             " diag_drop=%" PRIu32 " exhausted=%" PRIu32
             " deadline_miss=%" PRIu32
             " switches=%" PRIu32 " heap=%u stack_min=%u words",
             s_counters.yin_time_us, s_counters.mic1_fft_time_us,
             s_counters.chord_time_us, s_counters.low_yin_time_us,
             s_counters.low_spectrum_time_us,
             s_counters.dsp_cycle_time_us, average_us,
             s_counters.dsp_cycle_max_us, queue_depth, s_counters.dropped_buffer_count,
             s_counters.diagnostic_drop_count,
             s_counters.buffer_exhaustion_count, s_counters.dsp_deadline_miss_count,
             s_counters.mic_switch_count, free_heap, stack_words);
#endif
}
