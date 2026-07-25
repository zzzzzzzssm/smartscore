#include "score_data.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    SemaphoreHandle_t lock;
    char title[SCORE_DATA_TITLE_MAX_LENGTH];
    uint16_t bpm;
    score_note_t *notes;
    size_t note_count;
} score_store_t;

static score_store_t s_store;

static score_note_t *allocate_notes(size_t count)
{
    size_t bytes = count * sizeof(score_note_t);
    score_note_t *notes = NULL;
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        notes = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (notes == NULL) {
        notes = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return notes;
}

esp_err_t score_data_init(void)
{
    if (s_store.lock != NULL) {
        return ESP_OK;
    }
    memset(&s_store, 0, sizeof(s_store));
    s_store.lock = xSemaphoreCreateMutex();
    return s_store.lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t score_data_replace(const char *title,
                             uint16_t bpm,
                             score_note_t *notes,
                             size_t note_count)
{
    if (s_store.lock == NULL || title == NULL || notes == NULL ||
        note_count == 0 || note_count > SCORE_DATA_MAX_NOTES) {
        return ESP_ERR_INVALID_ARG;
    }
    score_note_t *old_notes;
    xSemaphoreTake(s_store.lock, portMAX_DELAY);
    old_notes = s_store.notes;
    s_store.notes = notes;
    s_store.note_count = note_count;
    s_store.bpm = bpm;
    strlcpy(s_store.title, title, sizeof(s_store.title));
    xSemaphoreGive(s_store.lock);
    heap_caps_free(old_notes);
    return ESP_OK;
}

esp_err_t score_data_copy(score_document_t *out_document)
{
    if (s_store.lock == NULL || out_document == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_document, 0, sizeof(*out_document));
    xSemaphoreTake(s_store.lock, portMAX_DELAY);
    if (s_store.note_count == 0 || s_store.notes == NULL) {
        xSemaphoreGive(s_store.lock);
        return ESP_ERR_NOT_FOUND;
    }
    score_note_t *copy = allocate_notes(s_store.note_count);
    if (copy == NULL) {
        xSemaphoreGive(s_store.lock);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, s_store.notes, s_store.note_count * sizeof(*copy));
    out_document->notes = copy;
    out_document->note_count = s_store.note_count;
    out_document->bpm = s_store.bpm;
    strlcpy(out_document->title, s_store.title, sizeof(out_document->title));
    xSemaphoreGive(s_store.lock);
    return ESP_OK;
}

void score_data_get_status(score_data_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }
    memset(out_status, 0, sizeof(*out_status));
    if (s_store.lock == NULL) {
        return;
    }
    xSemaphoreTake(s_store.lock, portMAX_DELAY);
    out_status->initialized = true;
    out_status->loaded = s_store.note_count > 0 && s_store.notes != NULL;
    out_status->note_count = s_store.note_count;
    out_status->bpm = s_store.bpm;
    strlcpy(out_status->title, s_store.title, sizeof(out_status->title));
    xSemaphoreGive(s_store.lock);
}

void score_document_release(score_document_t *document)
{
    if (document == NULL) {
        return;
    }
    heap_caps_free(document->notes);
    memset(document, 0, sizeof(*document));
}
