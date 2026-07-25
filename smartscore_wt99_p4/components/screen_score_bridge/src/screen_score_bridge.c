#include "screen_score_bridge.h"

#include <stdint.h>

#include "score_data.h"
#include "score_json_parser.h"

static uint32_t scale_milliseconds(uint32_t value,
                                   uint16_t source_bpm,
                                   uint16_t target_bpm)
{
    uint64_t scaled = ((uint64_t)value * source_bpm + target_bpm / 2U) /
                      target_bpm;
    return scaled > UINT32_MAX ? UINT32_MAX : (uint32_t)scaled;
}

esp_err_t screen_score_bridge_parse_and_store(const char *json,
                                               size_t length,
                                               int practice_bpm,
                                               char *error,
                                               size_t error_length,
                                               size_t *out_note_count)
{
    esp_err_t err = score_json_parse_and_store(json, length, error,
                                               error_length,
                                               out_note_count);
    if (err != ESP_OK || practice_bpm <= 0) {
        return err;
    }
    if (practice_bpm < 20 || practice_bpm > 400) {
        return ESP_ERR_INVALID_ARG;
    }

    score_data_status_t status;
    score_data_get_status(&status);
    if (!status.loaded || status.bpm == (uint16_t)practice_bpm) {
        return ESP_OK;
    }

    score_document_t document;
    err = score_data_copy(&document);
    if (err != ESP_OK) {
        return err;
    }
    for (size_t index = 0; index < document.note_count; ++index) {
        document.notes[index].start_ms = scale_milliseconds(
            document.notes[index].start_ms, document.bpm,
            (uint16_t)practice_bpm);
        document.notes[index].duration_ms = scale_milliseconds(
            document.notes[index].duration_ms, document.bpm,
            (uint16_t)practice_bpm);
        if (document.notes[index].duration_ms == 0) {
            document.notes[index].duration_ms = 1;
        }
    }

    err = score_data_replace(document.title, (uint16_t)practice_bpm,
                             document.notes, document.note_count);
    if (err == ESP_OK) {
        document.notes = NULL;
        document.note_count = 0;
    }
    score_document_release(&document);
    return err;
}
