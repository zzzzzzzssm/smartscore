#include "score_json_parser.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "score_data.h"

static void set_error(char *error,
                      size_t error_length,
                      const char *message)
{
    if (error != NULL && error_length > 0) {
        snprintf(error, error_length, "%s",
                 message != NULL ? message : "score_parse_failed");
    }
}

static int compare_notes(const void *left, const void *right)
{
    const score_note_t *a = (const score_note_t *)left;
    const score_note_t *b = (const score_note_t *)right;
    if (a->start_ms != b->start_ms) {
        return a->start_ms < b->start_ms ? -1 : 1;
    }
    if (a->midi != b->midi) {
        return a->midi < b->midi ? -1 : 1;
    }
    return 0;
}

static bool seconds_to_ms(const cJSON *item,
                          bool allow_zero,
                          uint32_t *out_value)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < 0.0 ||
        (!allow_zero && item->valuedouble <= 0.0)) {
        return false;
    }
    double milliseconds = item->valuedouble * 1000.0;
    if (milliseconds > (double)UINT32_MAX) {
        return false;
    }
    uint32_t value = (uint32_t)(milliseconds + 0.5);
    if (!allow_zero && value == 0) {
        value = 1;
    }
    *out_value = value;
    return true;
}

esp_err_t score_json_parse_and_store(const char *json,
                                     size_t length,
                                     char *error,
                                     size_t error_length,
                                     size_t *out_note_count)
{
    if (out_note_count != NULL) {
        *out_note_count = 0;
    }
    if (json == NULL || length == 0) {
        set_error(error, error_length, "empty_score_json");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_ParseWithLength(json, length);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        set_error(error, error_length, "invalid_score_json");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *notes_json = cJSON_GetObjectItemCaseSensitive(root, "notes");
    int count = cJSON_IsArray(notes_json) ? cJSON_GetArraySize(notes_json) : -1;
    if (count <= 0) {
        cJSON_Delete(root);
        set_error(error, error_length, "score_json_missing_notes");
        return ESP_ERR_INVALID_ARG;
    }
    if (count > SCORE_DATA_MAX_NOTES) {
        cJSON_Delete(root);
        set_error(error, error_length, "score_note_capacity_exceeded");
        return ESP_ERR_INVALID_SIZE;
    }

    score_note_t *notes = NULL;
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        notes = heap_caps_calloc((size_t)count, sizeof(*notes),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (notes == NULL) {
        notes = heap_caps_calloc((size_t)count, sizeof(*notes),
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (notes == NULL) {
        cJSON_Delete(root);
        set_error(error, error_length, "score_note_allocation_failed");
        return ESP_ERR_NO_MEM;
    }
    for (int index = 0; index < count; ++index) {
        cJSON *item = cJSON_GetArrayItem(notes_json, index);
        cJSON *midi = cJSON_GetObjectItemCaseSensitive(item, "midi");
        cJSON *start = cJSON_GetObjectItemCaseSensitive(item, "start");
        cJSON *duration = cJSON_GetObjectItemCaseSensitive(item, "duration");
        if (!cJSON_IsObject(item) || !cJSON_IsNumber(midi) ||
            midi->valuedouble != (double)midi->valueint ||
            midi->valueint < 0 || midi->valueint > 127 ||
            !seconds_to_ms(start, true, &notes[index].start_ms) ||
            !seconds_to_ms(duration, false, &notes[index].duration_ms)) {
            heap_caps_free(notes);
            cJSON_Delete(root);
            set_error(error, error_length, "invalid_score_note");
            return ESP_ERR_INVALID_ARG;
        }
        notes[index].midi = (uint8_t)midi->valueint;
    }
    qsort(notes, (size_t)count, sizeof(*notes), compare_notes);

    const char *title = "untitled";
    cJSON *title_json = cJSON_GetObjectItemCaseSensitive(root, "title");
    if (title_json != NULL) {
        if (!cJSON_IsString(title_json) || title_json->valuestring == NULL ||
            strlen(title_json->valuestring) >= SCORE_DATA_TITLE_MAX_LENGTH) {
            heap_caps_free(notes);
            cJSON_Delete(root);
            set_error(error, error_length, "invalid_score_title");
            return ESP_ERR_INVALID_ARG;
        }
        title = title_json->valuestring;
    }
    uint16_t bpm = 120;
    cJSON *bpm_json = cJSON_GetObjectItemCaseSensitive(root, "bpm");
    if (bpm_json != NULL) {
        if (!cJSON_IsNumber(bpm_json) || bpm_json->valueint < 20 ||
            bpm_json->valueint > 400) {
            heap_caps_free(notes);
            cJSON_Delete(root);
            set_error(error, error_length, "invalid_score_bpm");
            return ESP_ERR_INVALID_ARG;
        }
        bpm = (uint16_t)bpm_json->valueint;
    }

    char title_copy[SCORE_DATA_TITLE_MAX_LENGTH];
    strlcpy(title_copy, title, sizeof(title_copy));
    cJSON_Delete(root);
    esp_err_t result = score_data_replace(title_copy, bpm, notes,
                                          (size_t)count);
    if (result != ESP_OK) {
        heap_caps_free(notes);
        set_error(error, error_length, "score_store_failed");
        return result;
    }
    if (out_note_count != NULL) {
        *out_note_count = (size_t)count;
    }
    if (error != NULL && error_length > 0) {
        error[0] = '\0';
    }
    return ESP_OK;
}
