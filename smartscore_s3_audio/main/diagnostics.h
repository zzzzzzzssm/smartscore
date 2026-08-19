#pragma once

#include <stdint.h>

typedef struct {
    volatile uint32_t dropped_buffer_count;
    volatile uint32_t i2s_read_error_count;
    volatile uint32_t dsp_deadline_miss_count;
    volatile uint32_t queue_overflow_count;
    volatile uint32_t buffer_exhaustion_count;
    volatile uint32_t yin_time_us;
    volatile uint32_t mic1_fft_time_us;
    volatile uint32_t mic2_fft_time_us;
    volatile uint32_t chord_time_us;
    volatile uint32_t low_yin_time_us;
    volatile uint32_t low_spectrum_time_us;
    volatile uint32_t diagnostic_drop_count;
    volatile uint32_t dsp_cycle_time_us;
    volatile uint32_t dsp_cycle_max_us;
    volatile uint64_t dsp_cycle_sum_us;
    volatile uint32_t dsp_cycle_count;
    volatile uint32_t mic_switch_count;
} diagnostics_counters_t;

diagnostics_counters_t *diagnostics_counters(void);
void diagnostics_log_audio(float mic1_rms, float mic1_peak, float mic1_clip,
                           float mic2_rms, float mic2_peak, float mic2_clip,
                           int selected_mic, float mic1_noise, float mic2_noise,
                           float mic1_gate, float mic2_gate);
void diagnostics_log_performance(unsigned queue_depth, unsigned free_heap,
                                 unsigned stack_words);
