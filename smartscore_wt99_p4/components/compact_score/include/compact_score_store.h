#pragma once

#include <stddef.h>

#include "compact_score.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COMPACT_SCORE_TASK_ID_CAPACITY 48U

typedef struct {
    bool initialized;
    bool loaded;
    char task_id[COMPACT_SCORE_TASK_ID_CAPACITY];
    compact_score_kind_t kind;
    size_t event_count;
    size_t sounding_note_count;
} compact_score_store_status_t;

esp_err_t compact_score_store_init(void);

/* Takes ownership of document on success. Duplicate task IDs are rejected. */
esp_err_t compact_score_store_replace(const char *task_id,
                                      compact_score_document_t *document);

void compact_score_store_get_status(compact_score_store_status_t *status);

#ifdef __cplusplus
}
#endif

