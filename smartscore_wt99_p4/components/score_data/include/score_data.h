#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCORE_DATA_MAX_NOTES 1024
#define SCORE_DATA_TITLE_MAX_LENGTH 96

typedef struct {
    uint8_t midi;
    uint32_t start_ms;
    uint32_t duration_ms;
} score_note_t;

typedef struct {
    char title[SCORE_DATA_TITLE_MAX_LENGTH];
    uint16_t bpm;
    score_note_t *notes;
    size_t note_count;
} score_document_t;

typedef struct {
    bool initialized;
    bool loaded;
    char title[SCORE_DATA_TITLE_MAX_LENGTH];
    uint16_t bpm;
    size_t note_count;
} score_data_status_t;

esp_err_t score_data_init(void);
esp_err_t score_data_replace(const char *title,
                             uint16_t bpm,
                             score_note_t *notes,
                             size_t note_count);
esp_err_t score_data_copy(score_document_t *out_document);
void score_data_get_status(score_data_status_t *out_status);
void score_document_release(score_document_t *document);

#ifdef __cplusplus
}
#endif
