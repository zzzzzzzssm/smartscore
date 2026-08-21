#include "compact_score.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#define COMPACT_SCORE_MAX_SYSTEMS 128U
#define COMPACT_SCORE_MAX_STAVES 16U
#define COMPACT_SCORE_MAX_BARS 2048U
#define COMPACT_SCORE_MAX_VOLTA 16U
#define COMPACT_SCORE_MAX_PITCHES 64U
#define COMPACT_SCORE_MAX_LYRIC_BYTES 1024U

static void set_error(compact_score_error_t *error,
                      compact_score_error_code_t code,
                      const char *path)
{
    if (error == NULL) {
        return;
    }
    error->code = code;
    snprintf(error->path, sizeof(error->path), "%s", path != NULL ? path : "");
}

static bool json_integer(const cJSON *item, int64_t minimum,
                         int64_t maximum, int64_t *out_value)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < (double)minimum ||
        item->valuedouble > (double)maximum) {
        return false;
    }
    double integer_part = 0.0;
    if (modf(item->valuedouble, &integer_part) != 0.0) {
        return false;
    }
    if (out_value != NULL) {
        *out_value = (int64_t)integer_part;
    }
    return true;
}

static void *checked_calloc(size_t count, size_t size)
{
    if (count == 0 || size == 0 || count > SIZE_MAX / size) {
        return NULL;
    }
    return calloc(count, size);
}

static char *duplicate_string(const char *text)
{
    if (text == NULL) {
        return NULL;
    }
    size_t length = strlen(text);
    if (length > COMPACT_SCORE_MAX_LYRIC_BYTES) {
        return NULL;
    }
    char *copy = malloc(length + 1U);
    if (copy != NULL) {
        memcpy(copy, text, length + 1U);
    }
    return copy;
}

static bool valid_nkey(const char *key)
{
    if (key == NULL || key[0] < 'A' || key[0] > 'G') {
        return false;
    }
    if (key[1] == '\0') {
        return true;
    }
    return (key[1] == '#' || key[1] == 'b') && key[2] == '\0';
}

static int compare_u8(const void *left, const void *right)
{
    uint8_t a = *(const uint8_t *)left;
    uint8_t b = *(const uint8_t *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static int compare_events(const void *left, const void *right)
{
    const compact_score_event_t *a = left;
    const compact_score_event_t *b = right;
    if (a->start != b->start) {
        return a->start < b->start ? -1 : 1;
    }
    if (a->voice != b->voice) {
        return a->voice < b->voice ? -1 : 1;
    }
    if (a->source_order != b->source_order) {
        return a->source_order < b->source_order ? -1 : 1;
    }
    return 0;
}

static bool events_are_sorted(const compact_score_event_t *events, size_t count)
{
    for (size_t index = 1; index < count; ++index) {
        if (compare_events(&events[index - 1U], &events[index]) > 0) {
            return false;
        }
    }
    return true;
}

static void free_bar(compact_score_bar_t *bar)
{
    if (bar == NULL) {
        return;
    }
    for (size_t index = 0; index < bar->event_count; ++index) {
        free(bar->events[index].midi);
        free(bar->events[index].lyric);
    }
    free(bar->events);
    free(bar->volta);
    memset(bar, 0, sizeof(*bar));
}

void compact_score_free(compact_score_document_t *document)
{
    if (document == NULL) {
        return;
    }
    for (size_t system_index = 0; system_index < document->system_count;
         ++system_index) {
        compact_score_system_t *system = &document->systems[system_index];
        for (size_t bar_index = 0; bar_index < system->bar_count; ++bar_index) {
            free_bar(&system->bars[bar_index]);
        }
        free(system->bars);
        for (size_t staff_index = 0; staff_index < system->staff_count;
             ++staff_index) {
            compact_score_staff_t *staff = &system->staves[staff_index];
            for (size_t bar_index = 0; bar_index < staff->bar_count; ++bar_index) {
                free_bar(&staff->bars[bar_index]);
            }
            free(staff->bars);
        }
        free(system->staves);
    }
    free(document->systems);
    free(document);
}

static compact_score_error_code_t parse_meta(const cJSON *root,
                                             compact_score_document_t *document,
                                             compact_score_error_t *error)
{
    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(root, "meta");
    if (!cJSON_IsObject(meta)) {
        set_error(error, COMPACT_SCORE_ERR_META, "meta");
        return COMPACT_SCORE_ERR_META;
    }

    const cJSON *ppq = cJSON_GetObjectItemCaseSensitive(meta, "ppq");
    int64_t value = 0;
    if (!json_integer(ppq, COMPACT_SCORE_PPQ, COMPACT_SCORE_PPQ, &value)) {
        set_error(error, COMPACT_SCORE_ERR_META, "meta.ppq");
        return COMPACT_SCORE_ERR_META;
    }
    document->meta.ppq = (uint8_t)value;

    const cJSON *time_signature = cJSON_GetObjectItemCaseSensitive(meta, "ts");
    if (cJSON_IsNull(time_signature)) {
        document->meta.has_time_signature = false;
    } else if (cJSON_IsArray(time_signature) &&
               cJSON_GetArraySize(time_signature) == 2) {
        int64_t numerator = 0;
        int64_t denominator = 0;
        if (!json_integer(cJSON_GetArrayItem(time_signature, 0), 1, 64,
                          &numerator) ||
            !json_integer(cJSON_GetArrayItem(time_signature, 1), 1, 64,
                          &denominator) ||
            ((uint64_t)numerator * COMPACT_SCORE_PPQ * 4U) %
                    (uint64_t)denominator != 0U) {
            set_error(error, COMPACT_SCORE_ERR_META, "meta.ts");
            return COMPACT_SCORE_ERR_META;
        }
        document->meta.has_time_signature = true;
        document->meta.time_signature_numerator = (uint8_t)numerator;
        document->meta.time_signature_denominator = (uint8_t)denominator;
        document->measure_ticks =
            (uint32_t)((uint64_t)numerator * COMPACT_SCORE_PPQ * 4U /
                       (uint64_t)denominator);
    } else {
        set_error(error, COMPACT_SCORE_ERR_META, "meta.ts");
        return COMPACT_SCORE_ERR_META;
    }

    const cJSON *bpm = cJSON_GetObjectItemCaseSensitive(meta, "bpm");
    if (cJSON_IsNull(bpm)) {
        document->meta.has_bpm = false;
    } else if (json_integer(bpm, 1, 1000, &value)) {
        document->meta.has_bpm = true;
        document->meta.bpm = (uint16_t)value;
    } else {
        set_error(error, COMPACT_SCORE_ERR_META, "meta.bpm");
        return COMPACT_SCORE_ERR_META;
    }

    const cJSON *nkey = cJSON_GetObjectItemCaseSensitive(meta, "nkey");
    const cJSON *key_signature = cJSON_GetObjectItemCaseSensitive(meta, "ks");
    if (document->kind == COMPACT_SCORE_KIND_NUMBERED) {
        if (!cJSON_IsNull(key_signature)) {
            set_error(error, COMPACT_SCORE_ERR_META, "meta.ks");
            return COMPACT_SCORE_ERR_META;
        }
        if (cJSON_IsNull(nkey)) {
            document->meta.has_nkey = false;
        } else if (cJSON_IsString(nkey) && nkey->valuestring != NULL &&
                   valid_nkey(nkey->valuestring)) {
            document->meta.has_nkey = true;
            snprintf(document->meta.nkey, sizeof(document->meta.nkey), "%s",
                     nkey->valuestring);
        } else {
            set_error(error, COMPACT_SCORE_ERR_META, "meta.nkey");
            return COMPACT_SCORE_ERR_META;
        }
    } else {
        if (!cJSON_IsNull(nkey)) {
            set_error(error, COMPACT_SCORE_ERR_META, "meta.nkey");
            return COMPACT_SCORE_ERR_META;
        }
        if (cJSON_IsNull(key_signature)) {
            document->meta.has_key_signature = false;
        } else if (json_integer(key_signature, -7, 7, &value)) {
            document->meta.has_key_signature = true;
            document->meta.key_signature = (int8_t)value;
        } else {
            set_error(error, COMPACT_SCORE_ERR_META, "meta.ks");
            return COMPACT_SCORE_ERR_META;
        }
    }
    return COMPACT_SCORE_OK;
}

static compact_score_error_code_t parse_volta(const cJSON *bar_json,
                                              compact_score_bar_t *bar,
                                              const char *path,
                                              compact_score_error_t *error)
{
    const cJSON *volta = cJSON_GetObjectItemCaseSensitive(bar_json, "volta");
    if (!cJSON_IsArray(volta)) {
        set_error(error, COMPACT_SCORE_ERR_BARS, path);
        return COMPACT_SCORE_ERR_BARS;
    }
    int count = cJSON_GetArraySize(volta);
    if (count < 0 || (size_t)count > COMPACT_SCORE_MAX_VOLTA) {
        set_error(error, COMPACT_SCORE_ERR_CAPACITY, path);
        return COMPACT_SCORE_ERR_CAPACITY;
    }
    if (count == 0) {
        return COMPACT_SCORE_OK;
    }
    bar->volta = checked_calloc((size_t)count, sizeof(*bar->volta));
    if (bar->volta == NULL) {
        set_error(error, COMPACT_SCORE_ERR_NO_MEMORY, path);
        return COMPACT_SCORE_ERR_NO_MEMORY;
    }
    bar->volta_count = (size_t)count;
    for (int index = 0; index < count; ++index) {
        int64_t value = 0;
        if (!json_integer(cJSON_GetArrayItem(volta, index), 1, UINT8_MAX,
                          &value)) {
            set_error(error, COMPACT_SCORE_ERR_RANGE, path);
            return COMPACT_SCORE_ERR_RANGE;
        }
        bar->volta[index] = (uint8_t)value;
    }
    return COMPACT_SCORE_OK;
}

static compact_score_error_code_t parse_event(
    const cJSON *event_json,
    compact_score_kind_t kind,
    uint32_t measure_ticks,
    uint32_t source_order,
    compact_score_event_t *event,
    const char *path,
    compact_score_error_t *error)
{
    int expected_length = kind == COMPACT_SCORE_KIND_NUMBERED ? 7 : 6;
    if (!cJSON_IsArray(event_json) ||
        cJSON_GetArraySize(event_json) != expected_length) {
        set_error(error, COMPACT_SCORE_ERR_EVENT, path);
        return COMPACT_SCORE_ERR_EVENT;
    }

    int64_t start = 0;
    int64_t ticks = 0;
    if (!json_integer(cJSON_GetArrayItem(event_json, 0), 0, UINT32_MAX,
                      &start)) {
        set_error(error, COMPACT_SCORE_ERR_RANGE, path);
        return COMPACT_SCORE_ERR_RANGE;
    }

    int ticks_index = kind == COMPACT_SCORE_KIND_NUMBERED ? 3 : 2;
    if (!json_integer(cJSON_GetArrayItem(event_json, ticks_index), 1,
                      UINT32_MAX, &ticks)) {
        set_error(error, COMPACT_SCORE_ERR_RANGE, path);
        return COMPACT_SCORE_ERR_RANGE;
    }
    if ((uint64_t)start + (uint64_t)ticks > UINT32_MAX) {
        set_error(error, COMPACT_SCORE_ERR_RANGE, path);
        return COMPACT_SCORE_ERR_RANGE;
    }
    if (measure_ticks > 0U &&
        (uint64_t)start + (uint64_t)ticks > measure_ticks) {
        set_error(error, COMPACT_SCORE_ERR_MEASURE_OVERFLOW, path);
        return COMPACT_SCORE_ERR_MEASURE_OVERFLOW;
    }

    event->start = (uint32_t)start;
    event->ticks = (uint32_t)ticks;
    event->source_order = source_order;

    int64_t value = 0;
    int flags_index = kind == COMPACT_SCORE_KIND_NUMBERED ? 5 : 4;
    if (!json_integer(cJSON_GetArrayItem(event_json, flags_index), 0, 15,
                      &value)) {
        set_error(error, COMPACT_SCORE_ERR_FLAGS, path);
        return COMPACT_SCORE_ERR_FLAGS;
    }
    event->flags = (uint8_t)value;

    int lyric_index = kind == COMPACT_SCORE_KIND_NUMBERED ? 6 : 5;
    const cJSON *lyric = cJSON_GetArrayItem(event_json, lyric_index);
    if (!cJSON_IsString(lyric) || lyric->valuestring == NULL ||
        strlen(lyric->valuestring) > COMPACT_SCORE_MAX_LYRIC_BYTES) {
        set_error(error, COMPACT_SCORE_ERR_EVENT, path);
        return COMPACT_SCORE_ERR_EVENT;
    }
    event->lyric = duplicate_string(lyric->valuestring);
    if (event->lyric == NULL) {
        set_error(error, COMPACT_SCORE_ERR_NO_MEMORY, path);
        return COMPACT_SCORE_ERR_NO_MEMORY;
    }

    if (kind == COMPACT_SCORE_KIND_NUMBERED) {
        int64_t degree = 0;
        int64_t octave = 0;
        int64_t accidental = 0;
        if (!json_integer(cJSON_GetArrayItem(event_json, 1), 0, 7, &degree) ||
            !json_integer(cJSON_GetArrayItem(event_json, 2), -2, 2, &octave) ||
            !json_integer(cJSON_GetArrayItem(event_json, 4), 0, 5,
                          &accidental)) {
            set_error(error, COMPACT_SCORE_ERR_RANGE, path);
            return COMPACT_SCORE_ERR_RANGE;
        }
        event->degree = (int8_t)degree;
        event->octave = (int8_t)octave;
        event->accidental = (uint8_t)accidental;
        event->voice = 1;
    } else {
        const cJSON *pitches = cJSON_GetArrayItem(event_json, 1);
        int pitch_count = cJSON_IsArray(pitches) ? cJSON_GetArraySize(pitches) : -1;
        if (pitch_count < 0 || (size_t)pitch_count > COMPACT_SCORE_MAX_PITCHES) {
            set_error(error, COMPACT_SCORE_ERR_EVENT, path);
            return COMPACT_SCORE_ERR_EVENT;
        }
        if (pitch_count > 0) {
            event->midi = checked_calloc((size_t)pitch_count,
                                         sizeof(*event->midi));
            if (event->midi == NULL) {
                set_error(error, COMPACT_SCORE_ERR_NO_MEMORY, path);
                return COMPACT_SCORE_ERR_NO_MEMORY;
            }
            event->midi_count = (size_t)pitch_count;
            for (int pitch_index = 0; pitch_index < pitch_count; ++pitch_index) {
                int64_t pitch = 0;
                if (!json_integer(cJSON_GetArrayItem(pitches, pitch_index), 0,
                                  127, &pitch)) {
                    set_error(error, COMPACT_SCORE_ERR_RANGE, path);
                    return COMPACT_SCORE_ERR_RANGE;
                }
                event->midi[pitch_index] = (uint8_t)pitch;
            }
            qsort(event->midi, event->midi_count, sizeof(*event->midi),
                  compare_u8);
        }
        if (!json_integer(cJSON_GetArrayItem(event_json, 3), 1, UINT16_MAX,
                          &value)) {
            set_error(error, COMPACT_SCORE_ERR_RANGE, path);
            return COMPACT_SCORE_ERR_RANGE;
        }
        event->voice = (uint16_t)value;
    }
    return COMPACT_SCORE_OK;
}

static compact_score_error_code_t parse_bar(
    const cJSON *bar_json,
    compact_score_kind_t kind,
    uint32_t measure_ticks,
    uint32_t *source_order,
    compact_score_bar_t *bar,
    const char *path,
    compact_score_error_t *error)
{
    if (!cJSON_IsObject(bar_json)) {
        set_error(error, COMPACT_SCORE_ERR_BARS, path);
        return COMPACT_SCORE_ERR_BARS;
    }
    int64_t flags = 0;
    if (!json_integer(cJSON_GetObjectItemCaseSensitive(bar_json, "bf"), 0,
                      15, &flags)) {
        set_error(error, COMPACT_SCORE_ERR_FLAGS, path);
        return COMPACT_SCORE_ERR_FLAGS;
    }
    bar->flags = (uint8_t)flags;
    compact_score_error_code_t result =
        parse_volta(bar_json, bar, path, error);
    if (result != COMPACT_SCORE_OK) {
        return result;
    }

    const cJSON *events = cJSON_GetObjectItemCaseSensitive(bar_json, "e");
    if (!cJSON_IsArray(events)) {
        set_error(error, COMPACT_SCORE_ERR_BARS, path);
        return COMPACT_SCORE_ERR_BARS;
    }
    int count = cJSON_GetArraySize(events);
    if (count < 0 || (uint64_t)*source_order + (uint64_t)count >
                         COMPACT_SCORE_MAX_EVENTS) {
        set_error(error, COMPACT_SCORE_ERR_CAPACITY, path);
        return COMPACT_SCORE_ERR_CAPACITY;
    }
    if (count == 0) {
        return COMPACT_SCORE_OK;
    }
    bar->events = checked_calloc((size_t)count, sizeof(*bar->events));
    if (bar->events == NULL) {
        set_error(error, COMPACT_SCORE_ERR_NO_MEMORY, path);
        return COMPACT_SCORE_ERR_NO_MEMORY;
    }
    bar->event_count = (size_t)count;
    for (int index = 0; index < count; ++index) {
        char event_path[96];
        snprintf(event_path, sizeof(event_path), "%s.e[%d]", path, index);
        result = parse_event(cJSON_GetArrayItem(events, index), kind,
                             measure_ticks, (*source_order)++,
                             &bar->events[index], event_path, error);
        if (result != COMPACT_SCORE_OK) {
            return result;
        }
    }
    if (!events_are_sorted(bar->events, bar->event_count)) {
        qsort(bar->events, bar->event_count, sizeof(*bar->events),
              compare_events);
        return COMPACT_SCORE_OK;
    }
    return COMPACT_SCORE_OK;
}

static uint32_t bar_max_end(const compact_score_bar_t *bar)
{
    uint32_t maximum = 0;
    for (size_t index = 0; index < bar->event_count; ++index) {
        uint32_t end = bar->events[index].start + bar->events[index].ticks;
        if (end > maximum) {
            maximum = end;
        }
    }
    return maximum;
}

static compact_score_error_code_t validate_complete_bar(
    const compact_score_bar_t *bar,
    uint32_t measure_ticks,
    const char *path,
    compact_score_error_t *error)
{
    if (measure_ticks == 0U) {
        return COMPACT_SCORE_OK;
    }
    if (bar->event_count == 0U) {
        set_error(error, COMPACT_SCORE_ERR_INCOMPLETE_MEASURE, path);
        return COMPACT_SCORE_ERR_INCOMPLETE_MEASURE;
    }
    for (size_t index = 0; index < bar->event_count; ++index) {
        uint16_t voice = bar->events[index].voice;
        bool seen_before = false;
        uint32_t voice_end = 0;
        for (size_t other = 0; other < bar->event_count; ++other) {
            if (bar->events[other].voice == voice) {
                if (other < index) {
                    seen_before = true;
                }
                uint32_t end = bar->events[other].start +
                               bar->events[other].ticks;
                if (end > voice_end) {
                    voice_end = end;
                }
            }
        }
        if (!seen_before && voice_end != measure_ticks) {
            set_error(error, COMPACT_SCORE_ERR_INCOMPLETE_MEASURE, path);
            return COMPACT_SCORE_ERR_INCOMPLETE_MEASURE;
        }
    }
    return COMPACT_SCORE_OK;
}

static bool numbered_bar_has_before(const compact_score_document_t *document,
                                    size_t system_index, size_t bar_index)
{
    if (bar_index > 0U) {
        return true;
    }
    for (size_t index = 0; index < system_index; ++index) {
        if (document->systems[index].bar_count > 0U) {
            return true;
        }
    }
    return false;
}

static bool numbered_bar_has_after(const compact_score_document_t *document,
                                   size_t system_index, size_t bar_index)
{
    if (bar_index + 1U < document->systems[system_index].bar_count) {
        return true;
    }
    for (size_t index = system_index + 1U; index < document->system_count;
         ++index) {
        if (document->systems[index].bar_count > 0U) {
            return true;
        }
    }
    return false;
}

static bool staff_bar_has_before(const compact_score_document_t *document,
                                size_t system_index, size_t staff_index,
                                size_t bar_index)
{
    if (bar_index > 0U) {
        return true;
    }
    for (size_t index = 0; index < system_index; ++index) {
        const compact_score_system_t *system = &document->systems[index];
        if (staff_index < system->staff_count &&
            system->staves[staff_index].bar_count > 0U) {
            return true;
        }
    }
    return false;
}

static bool staff_bar_has_after(const compact_score_document_t *document,
                               size_t system_index, size_t staff_index,
                               size_t bar_index)
{
    const compact_score_staff_t *staff =
        &document->systems[system_index].staves[staff_index];
    if (bar_index + 1U < staff->bar_count) {
        return true;
    }
    for (size_t index = system_index + 1U; index < document->system_count;
         ++index) {
        const compact_score_system_t *system = &document->systems[index];
        if (staff_index < system->staff_count &&
            system->staves[staff_index].bar_count > 0U) {
            return true;
        }
    }
    return false;
}

static compact_score_error_code_t validate_interior_measures(
    const compact_score_document_t *document, compact_score_error_t *error)
{
    if (document->measure_ticks == 0U) {
        return COMPACT_SCORE_OK;
    }
    for (size_t system_index = 0; system_index < document->system_count;
         ++system_index) {
        const compact_score_system_t *system = &document->systems[system_index];
        if (document->kind == COMPACT_SCORE_KIND_NUMBERED) {
            for (size_t bar_index = 0; bar_index < system->bar_count; ++bar_index) {
                if (!numbered_bar_has_before(document, system_index, bar_index) ||
                    !numbered_bar_has_after(document, system_index, bar_index)) {
                    continue;
                }
                char path[96];
                snprintf(path, sizeof(path), "sys[%u].bars[%u]",
                         (unsigned)system_index, (unsigned)bar_index);
                compact_score_error_code_t result = validate_complete_bar(
                    &system->bars[bar_index], document->measure_ticks, path,
                    error);
                if (result != COMPACT_SCORE_OK) {
                    return result;
                }
            }
        } else {
            for (size_t staff_index = 0; staff_index < system->staff_count;
                 ++staff_index) {
                const compact_score_staff_t *staff = &system->staves[staff_index];
                for (size_t bar_index = 0; bar_index < staff->bar_count;
                     ++bar_index) {
                    if (!staff_bar_has_before(document, system_index, staff_index,
                                              bar_index) ||
                        !staff_bar_has_after(document, system_index, staff_index,
                                             bar_index)) {
                        continue;
                    }
                    char path[96];
                    snprintf(path, sizeof(path), "sys[%u].staves[%u].bars[%u]",
                             (unsigned)system_index, (unsigned)staff_index,
                             (unsigned)bar_index);
                    compact_score_error_code_t result = validate_complete_bar(
                        &staff->bars[bar_index], document->measure_ticks, path,
                        error);
                    if (result != COMPACT_SCORE_OK) {
                        return result;
                    }
                }
            }
        }
    }
    return COMPACT_SCORE_OK;
}

static compact_score_error_code_t assign_absolute_starts(
    compact_score_document_t *document, compact_score_error_t *error)
{
    uint64_t page_start = 0;
    uint32_t event_index = 0;
    document->event_count = 0;
    document->sounding_note_count = 0;

    for (size_t system_index = 0; system_index < document->system_count;
         ++system_index) {
        compact_score_system_t *system = &document->systems[system_index];
        if (document->kind == COMPACT_SCORE_KIND_NUMBERED) {
            uint64_t local_start = 0;
            for (size_t bar_index = 0; bar_index < system->bar_count; ++bar_index) {
                compact_score_bar_t *bar = &system->bars[bar_index];
                for (size_t index = 0; index < bar->event_count; ++index) {
                    compact_score_event_t *event = &bar->events[index];
                    uint64_t absolute = page_start + local_start + event->start;
                    if (absolute > UINT32_MAX) {
                        set_error(error, COMPACT_SCORE_ERR_RANGE,
                                  "absolute_start");
                        return COMPACT_SCORE_ERR_RANGE;
                    }
                    event->absolute_start = (uint32_t)absolute;
                    event->event_index = event_index++;
                    document->event_count++;
                    if (event->degree != 0) {
                        document->sounding_note_count++;
                    }
                }
                local_start += document->measure_ticks > 0U
                                   ? document->measure_ticks
                                   : bar_max_end(bar);
            }
            page_start += local_start;
        } else {
            size_t maximum_bars = 0;
            for (size_t staff_index = 0; staff_index < system->staff_count;
                 ++staff_index) {
                if (system->staves[staff_index].bar_count > maximum_bars) {
                    maximum_bars = system->staves[staff_index].bar_count;
                }
            }
            uint64_t local_start = 0;
            for (size_t bar_index = 0; bar_index < maximum_bars; ++bar_index) {
                uint32_t span = document->measure_ticks;
                if (span == 0U) {
                    for (size_t staff_index = 0; staff_index < system->staff_count;
                         ++staff_index) {
                        compact_score_staff_t *staff =
                            &system->staves[staff_index];
                        if (bar_index < staff->bar_count) {
                            uint32_t end = bar_max_end(&staff->bars[bar_index]);
                            if (end > span) {
                                span = end;
                            }
                        }
                    }
                }
                for (size_t staff_index = 0; staff_index < system->staff_count;
                     ++staff_index) {
                    compact_score_staff_t *staff = &system->staves[staff_index];
                    if (bar_index >= staff->bar_count) {
                        continue;
                    }
                    compact_score_bar_t *bar = &staff->bars[bar_index];
                    for (size_t index = 0; index < bar->event_count; ++index) {
                        compact_score_event_t *event = &bar->events[index];
                        uint64_t absolute =
                            page_start + local_start + event->start;
                        if (absolute > UINT32_MAX) {
                            set_error(error, COMPACT_SCORE_ERR_RANGE,
                                      "absolute_start");
                            return COMPACT_SCORE_ERR_RANGE;
                        }
                        event->absolute_start = (uint32_t)absolute;
                        event->event_index = event_index++;
                        document->event_count++;
                        document->sounding_note_count += event->midi_count;
                    }
                }
                local_start += span;
            }
            page_start += local_start;
        }
        if (page_start > UINT32_MAX) {
            set_error(error, COMPACT_SCORE_ERR_RANGE, "absolute_start");
            return COMPACT_SCORE_ERR_RANGE;
        }
    }
    if (document->event_count == 0U) {
        set_error(error, COMPACT_SCORE_ERR_EMPTY, "sys");
        return COMPACT_SCORE_ERR_EMPTY;
    }
    return COMPACT_SCORE_OK;
}

compact_score_error_code_t compact_score_parse(
    const char *json, size_t length, compact_score_document_t **out_document,
    compact_score_error_t *error)
{
    if (out_document != NULL) {
        *out_document = NULL;
    }
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }
    if (json == NULL || length == 0U || out_document == NULL) {
        set_error(error, COMPACT_SCORE_ERR_ARGUMENT, "json");
        return COMPACT_SCORE_ERR_ARGUMENT;
    }

    char *terminated = malloc(length + 1U);
    if (terminated == NULL) {
        set_error(error, COMPACT_SCORE_ERR_NO_MEMORY, "json");
        return COMPACT_SCORE_ERR_NO_MEMORY;
    }
    memcpy(terminated, json, length);
    terminated[length] = '\0';
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(terminated, length + 1U,
                                            &parse_end, true);
    free(terminated);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        set_error(error, COMPACT_SCORE_ERR_JSON, "json");
        return COMPACT_SCORE_ERR_JSON;
    }

    compact_score_document_t *document = calloc(1, sizeof(*document));
    if (document == NULL) {
        cJSON_Delete(root);
        set_error(error, COMPACT_SCORE_ERR_NO_MEMORY, "document");
        return COMPACT_SCORE_ERR_NO_MEMORY;
    }

    int64_t version = 0;
    if (!json_integer(cJSON_GetObjectItemCaseSensitive(root, "v"), 1, 1,
                      &version)) {
        compact_score_free(document);
        cJSON_Delete(root);
        set_error(error, COMPACT_SCORE_ERR_VERSION, "v");
        return COMPACT_SCORE_ERR_VERSION;
    }
    document->version = (uint8_t)version;
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(root, "kind");
    if (!cJSON_IsString(kind) || kind->valuestring == NULL ||
        (strcmp(kind->valuestring, "n") != 0 &&
         strcmp(kind->valuestring, "s") != 0)) {
        compact_score_free(document);
        cJSON_Delete(root);
        set_error(error, COMPACT_SCORE_ERR_KIND, "kind");
        return COMPACT_SCORE_ERR_KIND;
    }
    document->kind = strcmp(kind->valuestring, "n") == 0
                         ? COMPACT_SCORE_KIND_NUMBERED
                         : COMPACT_SCORE_KIND_STAFF;

    compact_score_error_code_t result = parse_meta(root, document, error);
    if (result != COMPACT_SCORE_OK) {
        compact_score_free(document);
        cJSON_Delete(root);
        return result;
    }

    const cJSON *systems = cJSON_GetObjectItemCaseSensitive(root, "sys");
    int system_count = cJSON_IsArray(systems) ? cJSON_GetArraySize(systems) : -1;
    if (system_count <= 0 || (size_t)system_count > COMPACT_SCORE_MAX_SYSTEMS) {
        compact_score_free(document);
        cJSON_Delete(root);
        set_error(error, system_count < 0 ? COMPACT_SCORE_ERR_SYSTEMS
                                          : COMPACT_SCORE_ERR_CAPACITY,
                  "sys");
        return error != NULL ? error->code : COMPACT_SCORE_ERR_SYSTEMS;
    }
    document->systems = checked_calloc((size_t)system_count,
                                       sizeof(*document->systems));
    if (document->systems == NULL) {
        compact_score_free(document);
        cJSON_Delete(root);
        set_error(error, COMPACT_SCORE_ERR_NO_MEMORY, "sys");
        return COMPACT_SCORE_ERR_NO_MEMORY;
    }
    document->system_count = (size_t)system_count;

    uint32_t source_order = 0;
    size_t total_bars = 0;
    for (int system_index = 0; system_index < system_count; ++system_index) {
        const cJSON *system_json = cJSON_GetArrayItem(systems, system_index);
        compact_score_system_t *system = &document->systems[system_index];
        if (!cJSON_IsObject(system_json)) {
            result = COMPACT_SCORE_ERR_SYSTEMS;
            set_error(error, result, "sys[]");
            break;
        }
        if (document->kind == COMPACT_SCORE_KIND_NUMBERED) {
            const cJSON *bars =
                cJSON_GetObjectItemCaseSensitive(system_json, "bars");
            int bar_count = cJSON_IsArray(bars) ? cJSON_GetArraySize(bars) : -1;
            if (bar_count <= 0 || total_bars + (size_t)bar_count >
                                      COMPACT_SCORE_MAX_BARS) {
                result = bar_count < 0 ? COMPACT_SCORE_ERR_BARS
                                       : COMPACT_SCORE_ERR_CAPACITY;
                set_error(error, result, "sys[].bars");
                break;
            }
            system->bars = checked_calloc((size_t)bar_count,
                                          sizeof(*system->bars));
            if (system->bars == NULL) {
                result = COMPACT_SCORE_ERR_NO_MEMORY;
                set_error(error, result, "sys[].bars");
                break;
            }
            system->bar_count = (size_t)bar_count;
            total_bars += (size_t)bar_count;
            for (int bar_index = 0; bar_index < bar_count; ++bar_index) {
                char path[96];
                snprintf(path, sizeof(path), "sys[%d].bars[%d]", system_index,
                         bar_index);
                result = parse_bar(cJSON_GetArrayItem(bars, bar_index),
                                   document->kind, document->measure_ticks,
                                   &source_order, &system->bars[bar_index], path,
                                   error);
                if (result != COMPACT_SCORE_OK) {
                    break;
                }
            }
        } else {
            const cJSON *staves =
                cJSON_GetObjectItemCaseSensitive(system_json, "staves");
            int staff_count =
                cJSON_IsArray(staves) ? cJSON_GetArraySize(staves) : -1;
            if (staff_count <= 0 ||
                (size_t)staff_count > COMPACT_SCORE_MAX_STAVES) {
                result = staff_count < 0 ? COMPACT_SCORE_ERR_STAVES
                                         : COMPACT_SCORE_ERR_CAPACITY;
                set_error(error, result, "sys[].staves");
                break;
            }
            system->staves = checked_calloc((size_t)staff_count,
                                            sizeof(*system->staves));
            if (system->staves == NULL) {
                result = COMPACT_SCORE_ERR_NO_MEMORY;
                set_error(error, result, "sys[].staves");
                break;
            }
            system->staff_count = (size_t)staff_count;
            for (int staff_index = 0; staff_index < staff_count; ++staff_index) {
                const cJSON *staff_json = cJSON_GetArrayItem(staves, staff_index);
                compact_score_staff_t *staff = &system->staves[staff_index];
                const cJSON *clef = cJSON_IsObject(staff_json)
                                        ? cJSON_GetObjectItemCaseSensitive(
                                              staff_json, "clef")
                                        : NULL;
                if (!cJSON_IsString(clef) || clef->valuestring == NULL ||
                    clef->valuestring[1] != '\0' ||
                    strchr("GFC", clef->valuestring[0]) == NULL) {
                    result = COMPACT_SCORE_ERR_STAVES;
                    set_error(error, result, "sys[].staves[].clef");
                    break;
                }
                staff->clef = clef->valuestring[0];
                const cJSON *bars =
                    cJSON_GetObjectItemCaseSensitive(staff_json, "bars");
                int bar_count =
                    cJSON_IsArray(bars) ? cJSON_GetArraySize(bars) : -1;
                if (bar_count <= 0 || total_bars + (size_t)bar_count >
                                          COMPACT_SCORE_MAX_BARS) {
                    result = bar_count < 0 ? COMPACT_SCORE_ERR_BARS
                                           : COMPACT_SCORE_ERR_CAPACITY;
                    set_error(error, result, "sys[].staves[].bars");
                    break;
                }
                staff->bars = checked_calloc((size_t)bar_count,
                                             sizeof(*staff->bars));
                if (staff->bars == NULL) {
                    result = COMPACT_SCORE_ERR_NO_MEMORY;
                    set_error(error, result, "sys[].staves[].bars");
                    break;
                }
                staff->bar_count = (size_t)bar_count;
                total_bars += (size_t)bar_count;
                for (int bar_index = 0; bar_index < bar_count; ++bar_index) {
                    char path[96];
                    snprintf(path, sizeof(path),
                             "sys[%d].staves[%d].bars[%d]", system_index,
                             staff_index, bar_index);
                    result = parse_bar(cJSON_GetArrayItem(bars, bar_index),
                                       document->kind, document->measure_ticks,
                                       &source_order, &staff->bars[bar_index],
                                       path, error);
                    if (result != COMPACT_SCORE_OK) {
                        break;
                    }
                }
                if (result != COMPACT_SCORE_OK) {
                    break;
                }
            }
        }
        if (result != COMPACT_SCORE_OK) {
            break;
        }
    }
    cJSON_Delete(root);
    if (result == COMPACT_SCORE_OK) {
        result = validate_interior_measures(document, error);
    }
    if (result == COMPACT_SCORE_OK) {
        result = assign_absolute_starts(document, error);
    }
    if (result != COMPACT_SCORE_OK) {
        compact_score_free(document);
        return result;
    }

    for (size_t system_index = 0; system_index < document->system_count;
         ++system_index) {
        compact_score_system_t *system = &document->systems[system_index];
        if (document->kind == COMPACT_SCORE_KIND_NUMBERED) {
            for (size_t bar_index = 0; bar_index < system->bar_count; ++bar_index) {
                compact_score_bar_t *bar = &system->bars[bar_index];
                for (size_t event_index_value = 0;
                     event_index_value < bar->event_count; ++event_index_value) {
                    if (bar->events[event_index_value].source_order !=
                        bar->events[event_index_value].event_index) {
                        document->reordered = true;
                    }
                }
            }
        } else {
            for (size_t staff_index = 0; staff_index < system->staff_count;
                 ++staff_index) {
                compact_score_staff_t *staff = &system->staves[staff_index];
                for (size_t bar_index = 0; bar_index < staff->bar_count;
                     ++bar_index) {
                    compact_score_bar_t *bar = &staff->bars[bar_index];
                    for (size_t event_index_value = 0;
                         event_index_value < bar->event_count;
                         ++event_index_value) {
                        if (bar->events[event_index_value].source_order !=
                            bar->events[event_index_value].event_index) {
                            document->reordered = true;
                        }
                    }
                }
            }
        }
    }

    *out_document = document;
    return COMPACT_SCORE_OK;
}

static int tonic_letter_index(char letter)
{
    switch (letter) {
    case 'C': return 0;
    case 'D': return 1;
    case 'E': return 2;
    case 'F': return 3;
    case 'G': return 4;
    case 'A': return 5;
    case 'B': return 6;
    default: return -1;
    }
}

static int natural_pitch_class_for_letter(int letter_index)
{
    static const int pitch_classes[] = {0, 2, 4, 5, 7, 9, 11};
    return pitch_classes[letter_index % 7];
}

static int nkey_pitch_class(const char *key)
{
    int letter = tonic_letter_index(key[0]);
    if (letter < 0) {
        return -1;
    }
    int pitch = natural_pitch_class_for_letter(letter);
    if (key[1] == '#') {
        pitch++;
    } else if (key[1] == 'b') {
        pitch--;
    }
    return (pitch + 12) % 12;
}

static bool numbered_event_midi(const compact_score_document_t *document,
                                const compact_score_event_t *event,
                                uint8_t *out_midi)
{
    static const int major_intervals[] = {0, 2, 4, 5, 7, 9, 11};
    int tonic_pc = nkey_pitch_class(document->meta.nkey);
    int tonic_letter = tonic_letter_index(document->meta.nkey[0]);
    if (tonic_pc < 0 || tonic_letter < 0 || event->degree < 1 ||
        event->degree > 7) {
        return false;
    }
    int base_tonic = 60 + tonic_pc;
    int semitones = major_intervals[event->degree - 1];
    switch (event->accidental) {
    case 0:
        break;
    case 1:
        semitones += 1;
        break;
    case 2:
        semitones -= 1;
        break;
    case 3: {
        int target_letter = (tonic_letter + event->degree - 1) % 7;
        int target_pc = natural_pitch_class_for_letter(target_letter);
        int natural_interval = (target_pc - tonic_pc + 12) % 12;
        semitones = natural_interval;
        break;
    }
    case 4:
        semitones += 2;
        break;
    case 5:
        semitones -= 2;
        break;
    default:
        return false;
    }
    int midi = base_tonic + semitones + 12 * event->octave;
    if (midi < 0 || midi > 127) {
        return false;
    }
    *out_midi = (uint8_t)midi;
    return true;
}

static uint32_t ticks_to_ms(uint32_t ticks, uint16_t bpm)
{
    uint64_t denominator = (uint64_t)bpm * COMPACT_SCORE_PPQ;
    uint64_t milliseconds = ((uint64_t)ticks * 60000U + denominator / 2U) /
                            denominator;
    if (milliseconds == 0U && ticks > 0U) {
        milliseconds = 1U;
    }
    return milliseconds > UINT32_MAX ? UINT32_MAX : (uint32_t)milliseconds;
}

static int compare_playback_notes(const void *left, const void *right)
{
    const compact_score_playback_note_t *a = left;
    const compact_score_playback_note_t *b = right;
    if (a->start_ms != b->start_ms) {
        return a->start_ms < b->start_ms ? -1 : 1;
    }
    if (a->staff != b->staff) {
        return a->staff < b->staff ? -1 : 1;
    }
    if (a->voice != b->voice) {
        return a->voice < b->voice ? -1 : 1;
    }
    if (a->midi != b->midi) {
        return a->midi < b->midi ? -1 : 1;
    }
    return 0;
}

static compact_score_error_code_t append_playback_event(
    const compact_score_document_t *document,
    const compact_score_event_t *event,
    uint16_t staff,
    uint16_t bpm,
    compact_score_playback_t *playback,
    size_t *index,
    compact_score_error_t *error)
{
    if (document->kind == COMPACT_SCORE_KIND_NUMBERED) {
        if (event->degree == 0) {
            return COMPACT_SCORE_OK;
        }
        uint8_t midi = 0;
        if (!numbered_event_midi(document, event, &midi)) {
            set_error(error, COMPACT_SCORE_ERR_UNPLAYABLE, "meta.nkey");
            return COMPACT_SCORE_ERR_UNPLAYABLE;
        }
        compact_score_playback_note_t *note = &playback->notes[(*index)++];
        note->midi = midi;
        note->flags = event->flags;
        note->staff = staff;
        note->voice = event->voice;
        note->event_index = event->event_index;
        note->start_ms = ticks_to_ms(event->absolute_start, bpm);
        note->duration_ms = ticks_to_ms(event->ticks, bpm);
        return COMPACT_SCORE_OK;
    }

    for (size_t pitch_index = 0; pitch_index < event->midi_count;
         ++pitch_index) {
        compact_score_playback_note_t *note = &playback->notes[(*index)++];
        note->midi = event->midi[pitch_index];
        note->flags = event->flags;
        note->staff = staff;
        note->voice = event->voice;
        note->event_index = event->event_index;
        note->start_ms = ticks_to_ms(event->absolute_start, bpm);
        note->duration_ms = ticks_to_ms(event->ticks, bpm);
    }
    return COMPACT_SCORE_OK;
}

compact_score_error_code_t compact_score_build_playback(
    const compact_score_document_t *document, uint16_t fallback_bpm,
    compact_score_playback_t *out_playback, compact_score_error_t *error)
{
    if (out_playback != NULL) {
        memset(out_playback, 0, sizeof(*out_playback));
    }
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
    }
    if (document == NULL || out_playback == NULL) {
        set_error(error, COMPACT_SCORE_ERR_ARGUMENT, "document");
        return COMPACT_SCORE_ERR_ARGUMENT;
    }
    if (document->kind == COMPACT_SCORE_KIND_NUMBERED &&
        !document->meta.has_nkey) {
        set_error(error, COMPACT_SCORE_ERR_UNPLAYABLE, "meta.nkey");
        return COMPACT_SCORE_ERR_UNPLAYABLE;
    }
    uint16_t bpm = document->meta.has_bpm ? document->meta.bpm : fallback_bpm;
    if (bpm == 0U) {
        set_error(error, COMPACT_SCORE_ERR_UNPLAYABLE, "meta.bpm");
        return COMPACT_SCORE_ERR_UNPLAYABLE;
    }
    if (document->sounding_note_count == 0U) {
        set_error(error, COMPACT_SCORE_ERR_UNPLAYABLE, "events");
        return COMPACT_SCORE_ERR_UNPLAYABLE;
    }
    out_playback->notes = checked_calloc(document->sounding_note_count,
                                         sizeof(*out_playback->notes));
    if (out_playback->notes == NULL) {
        set_error(error, COMPACT_SCORE_ERR_NO_MEMORY, "playback.notes");
        return COMPACT_SCORE_ERR_NO_MEMORY;
    }
    out_playback->bpm = bpm;

    size_t output_index = 0;
    compact_score_error_code_t result = COMPACT_SCORE_OK;
    for (size_t system_index = 0; system_index < document->system_count;
         ++system_index) {
        const compact_score_system_t *system = &document->systems[system_index];
        if (document->kind == COMPACT_SCORE_KIND_NUMBERED) {
            for (size_t bar_index = 0; bar_index < system->bar_count; ++bar_index) {
                const compact_score_bar_t *bar = &system->bars[bar_index];
                for (size_t event_index = 0; event_index < bar->event_count;
                     ++event_index) {
                    result = append_playback_event(
                        document, &bar->events[event_index], 1, bpm,
                        out_playback, &output_index, error);
                    if (result != COMPACT_SCORE_OK) {
                        goto fail;
                    }
                }
            }
        } else {
            for (size_t staff_index = 0; staff_index < system->staff_count;
                 ++staff_index) {
                const compact_score_staff_t *staff = &system->staves[staff_index];
                for (size_t bar_index = 0; bar_index < staff->bar_count;
                     ++bar_index) {
                    const compact_score_bar_t *bar = &staff->bars[bar_index];
                    for (size_t event_index = 0; event_index < bar->event_count;
                         ++event_index) {
                        result = append_playback_event(
                            document, &bar->events[event_index],
                            (uint16_t)(staff_index + 1U), bpm, out_playback,
                            &output_index, error);
                        if (result != COMPACT_SCORE_OK) {
                            goto fail;
                        }
                    }
                }
            }
        }
    }
    out_playback->note_count = output_index;
    qsort(out_playback->notes, out_playback->note_count,
          sizeof(*out_playback->notes), compare_playback_notes);
    return COMPACT_SCORE_OK;

fail:
    compact_score_playback_free(out_playback);
    return result;
}

void compact_score_playback_free(compact_score_playback_t *playback)
{
    if (playback == NULL) {
        return;
    }
    free(playback->notes);
    memset(playback, 0, sizeof(*playback));
}

const char *compact_score_error_name(compact_score_error_code_t code)
{
    switch (code) {
    case COMPACT_SCORE_OK: return "ok";
    case COMPACT_SCORE_ERR_ARGUMENT: return "invalid_argument";
    case COMPACT_SCORE_ERR_NO_MEMORY: return "no_memory";
    case COMPACT_SCORE_ERR_JSON: return "invalid_inner_json";
    case COMPACT_SCORE_ERR_VERSION: return "invalid_version";
    case COMPACT_SCORE_ERR_KIND: return "invalid_kind";
    case COMPACT_SCORE_ERR_META: return "invalid_meta";
    case COMPACT_SCORE_ERR_SYSTEMS: return "invalid_systems";
    case COMPACT_SCORE_ERR_STAVES: return "invalid_staves";
    case COMPACT_SCORE_ERR_BARS: return "invalid_bars";
    case COMPACT_SCORE_ERR_EVENT: return "invalid_event";
    case COMPACT_SCORE_ERR_RANGE: return "value_out_of_range";
    case COMPACT_SCORE_ERR_FLAGS: return "invalid_flags";
    case COMPACT_SCORE_ERR_MEASURE_OVERFLOW: return "measure_overflow";
    case COMPACT_SCORE_ERR_INCOMPLETE_MEASURE:
        return "incomplete_interior_measure";
    case COMPACT_SCORE_ERR_EMPTY: return "empty_score";
    case COMPACT_SCORE_ERR_CAPACITY: return "score_capacity_exceeded";
    case COMPACT_SCORE_ERR_UNPLAYABLE: return "playback_parameters_missing";
    default: return "unknown_compact_score_error";
    }
}
