#include "compact_score_store.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    SemaphoreHandle_t lock;
    char task_id[COMPACT_SCORE_TASK_ID_CAPACITY];
    compact_score_document_t *document;
} compact_score_store_t;

static compact_score_store_t s_store;

esp_err_t compact_score_store_init(void)
{
    if (s_store.lock != NULL) {
        return ESP_OK;
    }
    s_store.lock = xSemaphoreCreateMutex();
    return s_store.lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t compact_score_store_replace(const char *task_id,
                                      compact_score_document_t *document)
{
    if (s_store.lock == NULL || task_id == NULL || task_id[0] == '\0' ||
        strlen(task_id) >= sizeof(s_store.task_id) || document == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    compact_score_document_t *old_document = NULL;
    xSemaphoreTake(s_store.lock, portMAX_DELAY);
    if (strcmp(s_store.task_id, task_id) == 0) {
        xSemaphoreGive(s_store.lock);
        return ESP_ERR_INVALID_STATE;
    }
    old_document = s_store.document;
    s_store.document = document;
    strlcpy(s_store.task_id, task_id, sizeof(s_store.task_id));
    xSemaphoreGive(s_store.lock);
    compact_score_free(old_document);
    return ESP_OK;
}

void compact_score_store_get_status(compact_score_store_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (s_store.lock == NULL) {
        return;
    }

    xSemaphoreTake(s_store.lock, portMAX_DELAY);
    status->initialized = true;
    status->loaded = s_store.document != NULL;
    strlcpy(status->task_id, s_store.task_id, sizeof(status->task_id));
    if (s_store.document != NULL) {
        status->kind = s_store.document->kind;
        status->event_count = s_store.document->event_count;
        status->sounding_note_count = s_store.document->sounding_note_count;
    }
    xSemaphoreGive(s_store.lock);
}

