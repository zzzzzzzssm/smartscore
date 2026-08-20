#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIANO_SAMPLE_BANK_PATH "/sdcard/smartscore/piano.pbank"
#define PIANO_SAMPLE_BANK_RATE_HZ 24000U

typedef struct {
    uint8_t midi;
    uint8_t velocity;
} piano_sample_request_t;

typedef struct {
    const int16_t *samples;
    uint32_t sample_count;
    uint32_t loop_start;
    uint32_t loop_end;
    uint8_t root_midi;
    uint8_t velocity_min;
    uint8_t velocity_max;
    float gain;
} piano_sample_zone_t;

typedef struct piano_sample_bank piano_sample_bank_t;

typedef bool (*piano_sample_bank_cancel_cb_t)(void *user_data);

esp_err_t piano_sample_bank_load(
    const char *path,
    const piano_sample_request_t *requests,
    size_t request_count,
    piano_sample_bank_cancel_cb_t cancel_callback,
    void *cancel_user_data,
    piano_sample_bank_t **out_bank,
    char *error,
    size_t error_size);

const piano_sample_zone_t *piano_sample_bank_select(
    const piano_sample_bank_t *bank, uint8_t midi, uint8_t velocity);

size_t piano_sample_bank_memory_bytes(const piano_sample_bank_t *bank);
size_t piano_sample_bank_zone_count(const piano_sample_bank_t *bank);
void piano_sample_bank_free(piano_sample_bank_t *bank);

#ifdef __cplusplus
}
#endif
