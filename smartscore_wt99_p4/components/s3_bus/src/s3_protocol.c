#include "s3_protocol.h"

#include <math.h>
#include <string.h>

#include "cJSON.h"

static bool json_u32(const cJSON *object, const char *name, uint32_t *out_value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < 0.0 ||
        item->valuedouble > (double)UINT32_MAX) {
        return false;
    }
    const uint32_t value = (uint32_t)item->valuedouble;
    if ((double)value != item->valuedouble) {
        return false;
    }
    *out_value = value;
    return true;
}

static bool json_u8_range(const cJSON *object,
                          const char *name,
                          uint8_t minimum,
                          uint8_t maximum,
                          uint8_t *out_value)
{
    uint32_t value;
    if (!json_u32(object, name, &value) ||
        value < minimum || value > maximum) {
        return false;
    }
    *out_value = (uint8_t)value;
    return true;
}

static bool json_required_bool(const cJSON *object,
                               const char *name,
                               bool *out_value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsBool(item)) {
        return false;
    }
    *out_value = cJSON_IsTrue(item);
    return true;
}

static bool json_optional_float(const cJSON *object,
                                const char *name,
                                float minimum,
                                float maximum,
                                float *out_value,
                                bool *out_present)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    *out_present = item != NULL;
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < minimum || item->valuedouble > maximum) {
        return false;
    }
    *out_value = (float)item->valuedouble;
    return true;
}

static bool json_optional_u32(const cJSON *object,
                              const char *name,
                              uint32_t *out_value,
                              bool *out_present)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    *out_present = item != NULL;
    return item == NULL || json_u32(object, name, out_value);
}

static bool json_number_range(const cJSON *item, double minimum,
                              double maximum, float *out_value)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < minimum || item->valuedouble > maximum) {
        return false;
    }
    *out_value = (float)item->valuedouble;
    return true;
}

static bool parse_result_kind(const cJSON *root, const char *name,
                              s3_protocol_result_kind_t *out_kind)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL) return false;
    static const struct {
        const char *name;
        s3_protocol_result_kind_t kind;
    } kinds[] = {
        {"unknown", S3_PROTOCOL_RESULT_UNKNOWN},
        {"silence", S3_PROTOCOL_RESULT_SILENCE},
        {"single", S3_PROTOCOL_RESULT_SINGLE},
        {"interval", S3_PROTOCOL_RESULT_INTERVAL},
        {"chord", S3_PROTOCOL_RESULT_CHORD},
    };
    for (size_t index = 0; index < sizeof(kinds) / sizeof(kinds[0]); ++index) {
        if (strcmp(item->valuestring, kinds[index].name) == 0) {
            *out_kind = kinds[index].kind;
            return true;
        }
    }
    return false;
}

static bool parse_diagnostic_notes(const cJSON *root, const char *name,
                                   uint8_t *out_notes, uint8_t *out_count)
{
    const cJSON *notes = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsArray(notes)) return false;
    const int count = cJSON_GetArraySize(notes);
    if (count < 0 || count > S3_PROTOCOL_DIAGNOSTIC_MAX_NOTES) return false;
    for (int index = 0; index < count; ++index) {
        const cJSON *note = cJSON_GetArrayItem(notes, index);
        if (!cJSON_IsNumber(note) || !isfinite(note->valuedouble) ||
            note->valuedouble < 0.0 || note->valuedouble > 127.0 ||
            floor(note->valuedouble) != note->valuedouble) {
            return false;
        }
        const uint8_t midi = (uint8_t)note->valuedouble;
        for (int previous = 0; previous < index; ++previous) {
            if (out_notes[previous] == midi) return false;
        }
        out_notes[index] = midi;
    }
    *out_count = (uint8_t)count;
    return true;
}

static bool diagnostic_kind_matches_count(s3_protocol_result_kind_t kind,
                                          uint8_t count)
{
    switch (kind) {
        case S3_PROTOCOL_RESULT_UNKNOWN:
        case S3_PROTOCOL_RESULT_SILENCE:
            return count == 0;
        case S3_PROTOCOL_RESULT_SINGLE:
            return count == 1;
        case S3_PROTOCOL_RESULT_INTERVAL:
            return count == 2;
        case S3_PROTOCOL_RESULT_CHORD:
            return count >= 2 &&
                   count <= S3_PROTOCOL_DIAGNOSTIC_MAX_NOTES;
        default:
            return false;
    }
}

static bool parse_diagnostic_fields(const cJSON *root,
                                    s3_music_message_t *message)
{
    const cJSON *raw = cJSON_GetObjectItemCaseSensitive(root, "raw");
    if (!cJSON_IsArray(raw)) return false;
    const int raw_count = cJSON_GetArraySize(raw);
    if (raw_count < 0 || raw_count > S3_PROTOCOL_DIAGNOSTIC_MAX_RAW) {
        return false;
    }
    for (int index = 0; index < raw_count; ++index) {
        const cJSON *candidate = cJSON_GetArrayItem(raw, index);
        if (!cJSON_IsArray(candidate) ||
            cJSON_GetArraySize(candidate) != 4) {
            return false;
        }
        const cJSON *source = cJSON_GetArrayItem(candidate, 0);
        const cJSON *midi = cJSON_GetArrayItem(candidate, 1);
        if (!cJSON_IsNumber(source) || !isfinite(source->valuedouble) ||
            floor(source->valuedouble) != source->valuedouble ||
            source->valuedouble < 0.0 || source->valuedouble > 3.0 ||
            !cJSON_IsNumber(midi) || !isfinite(midi->valuedouble) ||
            floor(midi->valuedouble) != midi->valuedouble ||
            midi->valuedouble < 0.0 || midi->valuedouble > 127.0) {
            return false;
        }
        s3_protocol_diagnostic_candidate_t *out =
            &message->diagnostic_raw[index];
        out->source = (uint8_t)source->valuedouble;
        out->midi = (uint8_t)midi->valuedouble;
        if (!json_number_range(cJSON_GetArrayItem(candidate, 2),
                               0.0, 12000.0, &out->frequency_hz) ||
            !json_number_range(cJSON_GetArrayItem(candidate, 3),
                               0.0, 1.0, &out->confidence)) {
            return false;
        }
    }
    message->diagnostic_raw_count = (uint8_t)raw_count;
    if (!parse_result_kind(root, "candidate_kind",
                           &message->diagnostic_candidate_kind) ||
        !parse_diagnostic_notes(root, "candidate",
                                message->diagnostic_candidate_notes,
                                &message->diagnostic_candidate_count) ||
        !parse_result_kind(root, "final_kind",
                           &message->diagnostic_final_kind) ||
        !parse_diagnostic_notes(root, "final",
                                 message->diagnostic_final_notes,
                                 &message->diagnostic_final_count)) {
        return false;
    }
    if (!diagnostic_kind_matches_count(
            message->diagnostic_candidate_kind,
            message->diagnostic_candidate_count) ||
        !diagnostic_kind_matches_count(
            message->diagnostic_final_kind,
            message->diagnostic_final_count)) {
        return false;
    }
    const cJSON *reject = cJSON_GetObjectItemCaseSensitive(root, "reject");
    const cJSON *octave = cJSON_GetObjectItemCaseSensitive(root, "octave");
    const cJSON *snr = cJSON_GetObjectItemCaseSensitive(root, "snr");
    if (!cJSON_IsString(reject) || reject->valuestring == NULL ||
        strlen(reject->valuestring) >= sizeof(message->diagnostic_reject) ||
        !cJSON_IsNumber(octave) || !isfinite(octave->valuedouble) ||
        floor(octave->valuedouble) != octave->valuedouble ||
        octave->valuedouble < -24.0 || octave->valuedouble > 24.0 ||
        !cJSON_IsArray(snr) || cJSON_GetArraySize(snr) != 3) {
        return false;
    }
    memcpy(message->diagnostic_reject, reject->valuestring,
           strlen(reject->valuestring) + 1U);
    message->diagnostic_octave_shift = (int8_t)octave->valuedouble;
    for (int index = 0; index < 3; ++index) {
        if (!json_number_range(cJSON_GetArrayItem(snr, index),
                               -120.0, 120.0,
                               &message->diagnostic_snr_db[index])) {
            return false;
        }
    }
    return true;
}

static bool parse_poly_fields(const cJSON *root,
                              s3_music_message_t *message)
{
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(root, "kind");
    const cJSON *notes = cJSON_GetObjectItemCaseSensitive(root, "notes");
    if (!cJSON_IsString(kind) || kind->valuestring == NULL ||
        !cJSON_IsArray(notes)) {
        return false;
    }
    if (strcmp(kind->valuestring, "interval") == 0) {
        message->poly_kind = S3_PROTOCOL_POLY_INTERVAL;
    } else if (strcmp(kind->valuestring, "chord") == 0) {
        message->poly_kind = S3_PROTOCOL_POLY_CHORD;
    } else {
        return false;
    }

    const int count = cJSON_GetArraySize(notes);
    if (count < 2 || count > S3_PROTOCOL_POLY_MAX_NOTES) return false;
    for (int index = 0; index < count; ++index) {
        const cJSON *note = cJSON_GetArrayItem(notes, index);
        if (!cJSON_IsNumber(note) || !isfinite(note->valuedouble) ||
            note->valuedouble < 0.0 || note->valuedouble > 127.0 ||
            floor(note->valuedouble) != note->valuedouble) {
            return false;
        }
        const uint8_t midi = (uint8_t)note->valuedouble;
        for (int previous = 0; previous < index; ++previous) {
            if (message->notes[previous] == midi) return false;
        }
        message->notes[index] = midi;
    }
    message->note_count = (uint8_t)count;

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (name != NULL) {
        if (!cJSON_IsString(name) || name->valuestring == NULL ||
            strlen(name->valuestring) >= sizeof(message->poly_name)) {
            return false;
        }
        memcpy(message->poly_name, name->valuestring,
               strlen(name->valuestring) + 1U);
    }
    if (message->poly_kind == S3_PROTOCOL_POLY_CHORD &&
        message->poly_name[0] == '\0') {
        return false;
    }

    if (!json_optional_float(root, "confidence", 0.0f, 1.0f,
                             &message->confidence,
                             &message->has_confidence)) {
        return false;
    }
    if (!message->has_confidence) message->confidence = 0.75f;
    return true;
}

static bool parse_type(const char *type, s3_music_message_type_t *out_type)
{
    static const struct {
        const char *name;
        s3_music_message_type_t type;
    } known_types[] = {
        {"hello", S3_MUSIC_MESSAGE_HELLO},
        {"status", S3_MUSIC_MESSAGE_STATUS},
        {"pong", S3_MUSIC_MESSAGE_PONG},
        {"heartbeat", S3_MUSIC_MESSAGE_HEARTBEAT},
        {"pitch", S3_MUSIC_MESSAGE_PITCH},
        {"note_on", S3_MUSIC_MESSAGE_NOTE_ON},
        {"note_off", S3_MUSIC_MESSAGE_NOTE_OFF},
        {"poly", S3_MUSIC_MESSAGE_POLY},
        {"diagnostic", S3_MUSIC_MESSAGE_DIAGNOSTIC},
    };
    for (size_t i = 0; i < sizeof(known_types) / sizeof(known_types[0]); ++i) {
        if (strcmp(type, known_types[i].name) == 0) {
            *out_type = known_types[i].type;
            return true;
        }
    }
    return false;
}

s3_protocol_result_t s3_protocol_parse_music_line(
    const char *line,
    size_t length,
    s3_music_message_t *out_message)
{
    if (line == NULL || out_message == NULL || length == 0 ||
        line[length] != '\0') {
        return S3_PROTOCOL_INVALID_FIELD;
    }

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithOpts(line, &parse_end, true);
    if (root == NULL || !cJSON_IsObject(root) ||
        parse_end != line + length) {
        cJSON_Delete(root);
        return S3_PROTOCOL_INVALID_JSON;
    }

    s3_music_message_t message = {0};
    uint32_t version;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!json_u32(root, "v", &version) || version != 1 ||
        !cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(root);
        return S3_PROTOCOL_INVALID_FIELD;
    }
    if (!parse_type(type->valuestring, &message.type)) {
        cJSON_Delete(root);
        return S3_PROTOCOL_UNKNOWN_TYPE;
    }
    if (!json_u32(root, "seq", &message.seq) ||
        !json_u32(root, "sid", &message.sid) ||
        !json_u32(root, "ts_ms", &message.ts_ms)) {
        cJSON_Delete(root);
        return S3_PROTOCOL_INVALID_FIELD;
    }

    bool fields_valid = true;
    switch (message.type) {
    case S3_MUSIC_MESSAGE_HELLO: {
        const cJSON *source =
            cJSON_GetObjectItemCaseSensitive(root, "source");
        fields_valid = cJSON_IsString(source) &&
                       source->valuestring != NULL &&
                       strcmp(source->valuestring, "s3_audio") == 0;
        break;
    }
    case S3_MUSIC_MESSAGE_STATUS:
        fields_valid =
            json_required_bool(root, "ready", &message.ready) &&
            json_required_bool(root, "stream_enabled",
                               &message.stream_enabled);
        break;
    case S3_MUSIC_MESSAGE_HEARTBEAT:
        fields_valid = json_required_bool(root, "ready", &message.ready);
        break;
    case S3_MUSIC_MESSAGE_NOTE_ON:
        fields_valid =
            json_u8_range(root, "midi", 0, 127, &message.midi) &&
            json_u8_range(root, "velocity", 1, 127, &message.velocity) &&
            json_optional_float(root, "freq_hz", 0.0f, 24000.0f,
                                &message.frequency_hz,
                                &message.has_frequency) &&
            json_optional_float(root, "confidence", 0.0f, 1.0f,
                                &message.confidence,
                                &message.has_confidence);
        if (fields_valid && !message.has_confidence) {
            message.confidence = 0.75f;
        }
        break;
    case S3_MUSIC_MESSAGE_NOTE_OFF:
        fields_valid =
            json_u8_range(root, "midi", 0, 127, &message.midi) &&
            json_optional_u32(root, "duration_ms", &message.duration_ms,
                              &message.has_duration);
        message.velocity = 0;
        break;
    case S3_MUSIC_MESSAGE_PONG:
    case S3_MUSIC_MESSAGE_PITCH:
        break;
    case S3_MUSIC_MESSAGE_POLY:
        fields_valid = parse_poly_fields(root, &message);
        break;
    case S3_MUSIC_MESSAGE_DIAGNOSTIC:
        fields_valid = parse_diagnostic_fields(root, &message);
        break;
    }

    cJSON_Delete(root);
    if (!fields_valid) {
        return S3_PROTOCOL_INVALID_FIELD;
    }
    *out_message = message;
    return S3_PROTOCOL_OK;
}
