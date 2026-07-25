#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#define WAV_STREAM_MAX_NAME 256

typedef struct {
    FILE *file;
    uint32_t sample_rate_hz;
    uint32_t data_bytes;
    uint32_t remaining_bytes;
    char name[WAV_STREAM_MAX_NAME];
} wav_stream_file_t;

esp_err_t wav_stream_list(char names[][WAV_STREAM_MAX_NAME],
                          size_t capacity,
                          size_t *out_count);
esp_err_t wav_stream_list_page(const char *search,
                               size_t requested_page,
                               size_t page_size,
                               char names[][WAV_STREAM_MAX_NAME],
                               size_t capacity,
                               size_t *out_count,
                               size_t *out_total,
                               size_t *out_page);
esp_err_t wav_stream_open(const char *safe_name, wav_stream_file_t *out_file);
esp_err_t wav_stream_read(wav_stream_file_t *stream,
                          void *buffer,
                          size_t capacity,
                          size_t *out_bytes);
void wav_stream_close(wav_stream_file_t *stream);
