#include "score_json_parser.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_state.h"
#include "cJSON.h"

static int compare_target_note_start(const void *a, const void *b)
{
    const target_note_t *na = (const target_note_t *)a;
    const target_note_t *nb = (const target_note_t *)b;

    if (na->start < nb->start) return -1;
    if (na->start > nb->start) return 1;
    if (na->midi < nb->midi) return -1;
    if (na->midi > nb->midi) return 1;
    return 0;
}

static void set_error(char *error, size_t error_len, const char *message)
{
    if (error != NULL && error_len > 0) {
        snprintf(error, error_len, "%s", message != NULL ? message : "error");
    }
}

esp_err_t score_json_parse_and_store_with_tempo(const char *json, size_t len,
                                                 int tempo_bpm,
                                                 char *error,
                                                 size_t error_len)
{
    if (json == NULL || len == 0) {
        set_error(error, error_len, "empty score json");
        return ESP_ERR_INVALID_ARG;
    }

    char *copy = (char *)malloc(len + 1);
    if (copy == NULL) {
        set_error(error, error_len, "not enough memory for score json");
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, json, len);
    copy[len] = '\0';

    cJSON *root = cJSON_Parse(copy);
    free(copy);
    if (root == NULL) {
        set_error(error, error_len, "invalid score json");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *bpm_json = cJSON_GetObjectItemCaseSensitive(root, "bpm");
    int source_bpm = cJSON_IsNumber(bpm_json) ? bpm_json->valueint : 120;
    if (source_bpm <= 0) source_bpm = 120;
    int effective_bpm = tempo_bpm > 0 ? tempo_bpm : source_bpm;
    float tempo_scale = (float)source_bpm / (float)effective_bpm;

    cJSON *notes_json = cJSON_GetObjectItemCaseSensitive(root, "notes");
    if (!cJSON_IsArray(notes_json)) {
        cJSON_Delete(root);
        set_error(error, error_len, "score json missing notes");
        return ESP_ERR_INVALID_ARG;
    }

    target_note_t *notes = calloc(SCORE_MAX_TARGET_NOTES, sizeof(target_note_t));
    if (notes == NULL) {
        cJSON_Delete(root);
        set_error(error, error_len, "not enough memory for target notes");
        return ESP_ERR_NO_MEM;
    }

    int count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, notes_json) {
        if (count >= SCORE_MAX_TARGET_NOTES) {
            break;
        }

        cJSON *midi = cJSON_GetObjectItemCaseSensitive(item, "midi");
        cJSON *start = cJSON_GetObjectItemCaseSensitive(item, "start");
        cJSON *duration = cJSON_GetObjectItemCaseSensitive(item, "duration");
        if (!cJSON_IsNumber(midi) || !cJSON_IsNumber(start) || !cJSON_IsNumber(duration)) {
            continue;
        }

        int midi_value = midi->valueint;
        if (midi_value < 0 || midi_value > 127) {
            continue;
        }

        notes[count].midi = midi_value;
        notes[count].start = (float)start->valuedouble * tempo_scale;
        notes[count].duration = (float)duration->valuedouble * tempo_scale;
        if (notes[count].duration <= 0.01f) {
            notes[count].duration = 0.1f;
        }
        count++;
    }

    if (count <= 0) {
        free(notes);
        cJSON_Delete(root);
        set_error(error, error_len, "score json has no valid notes");
        return ESP_ERR_INVALID_ARG;
    }

    qsort(notes, (size_t)count, sizeof(target_note_t), compare_target_note_start);

    cJSON *title_json = cJSON_GetObjectItemCaseSensitive(root, "title");
    const char *title = cJSON_IsString(title_json) ? title_json->valuestring : "untitled";
    int bpm = effective_bpm;

    int stored_count = 0;
    bool truncated = false;
    esp_err_t err = app_state_set_score(title, bpm, notes, count, &stored_count, &truncated);
    free(notes);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        set_error(error, error_len, "failed to store score");
        return err;
    }
    if (truncated) {
        set_error(error, error_len, "score stored but truncated");
    }
    return ESP_OK;
}

esp_err_t score_json_parse_and_store(const char *json, size_t len,
                                     char *error, size_t error_len)
{
    return score_json_parse_and_store_with_tempo(json, len, 0,
                                                  error, error_len);
}
