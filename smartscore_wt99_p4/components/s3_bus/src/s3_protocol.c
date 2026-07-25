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
            json_u8_range(root, "velocity", 1, 127, &message.velocity);
        break;
    case S3_MUSIC_MESSAGE_NOTE_OFF:
        fields_valid = json_u8_range(root, "midi", 0, 127, &message.midi);
        message.velocity = 0;
        break;
    case S3_MUSIC_MESSAGE_PONG:
    case S3_MUSIC_MESSAGE_PITCH:
    case S3_MUSIC_MESSAGE_POLY:
        break;
    }

    cJSON_Delete(root);
    if (!fields_valid) {
        return S3_PROTOCOL_INVALID_FIELD;
    }
    *out_message = message;
    return S3_PROTOCOL_OK;
}
