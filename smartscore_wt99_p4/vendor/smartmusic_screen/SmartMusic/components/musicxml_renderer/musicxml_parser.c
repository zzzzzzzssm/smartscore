#include "musicxml_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <expat.h>

typedef struct {
    music_score_t *score;
    musicxml_error_t *error;
    XML_Parser parser;
    bool failed;

    int current_part;
    int current_measure;
    int32_t cursor;
    int32_t max_cursor;
    int32_t last_note_onset;

    int32_t divisions;
    uint8_t staff_count;
    music_clef_t clefs[MUSIC_MAX_STAVES];
    music_key_signature_t key;
    music_time_signature_t time;

    bool in_attributes;
    bool in_key;
    bool in_time;
    bool in_clef;
    bool in_note;
    bool in_pitch;
    bool in_backup;
    bool in_forward;
    bool in_barline;
    bool in_articulations;

    uint8_t attribute_staff;
    music_clef_t pending_clef;
    music_key_signature_t pending_key;
    music_time_signature_t pending_time;
    music_note_t note;
    bool note_is_rest;
    int beam_number;
    int32_t movement_duration;
    music_barline_t pending_barline;

    char text[128];
    size_t text_len;
} parser_ctx_t;

static const char *local_name(const char *name)
{
    const char *colon = strrchr(name, ':');
    return colon ? colon + 1 : name;
}

static const char *attribute_value(const XML_Char **attrs, const char *wanted)
{
    if (!attrs) return NULL;
    for (int i = 0; attrs[i] && attrs[i + 1]; i += 2) {
        if (strcmp(local_name(attrs[i]), wanted) == 0) return attrs[i + 1];
    }
    return NULL;
}

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static char *trim(char *text)
{
    while (*text && isspace((unsigned char)*text)) text++;
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) *--end = '\0';
    return text;
}

static void reset_text(parser_ctx_t *ctx)
{
    ctx->text_len = 0;
    ctx->text[0] = '\0';
}

static void fail(parser_ctx_t *ctx, const char *message)
{
    if (ctx->failed) return;
    ctx->failed = true;
    if (ctx->error) {
        ctx->error->line = XML_GetCurrentLineNumber(ctx->parser);
        ctx->error->column = XML_GetCurrentColumnNumber(ctx->parser);
        copy_string(ctx->error->message, sizeof(ctx->error->message), message);
    }
    XML_StopParser(ctx->parser, XML_FALSE);
}

static music_measure_t *current_measure(parser_ctx_t *ctx)
{
    if (ctx->current_measure < 0 ||
        ctx->current_measure >= ctx->score->measure_count) return NULL;
    return &ctx->score->measures[ctx->current_measure];
}

static music_event_t *append_event(parser_ctx_t *ctx,
                                   music_event_kind_t kind,
                                   int32_t onset)
{
    if (ctx->score->event_count >= MUSIC_MAX_EVENTS) {
        fail(ctx, "MusicXML exceeds MUSIC_MAX_EVENTS");
        return NULL;
    }
    music_measure_t *measure = current_measure(ctx);
    if (!measure) {
        fail(ctx, "MusicXML event outside a measure");
        return NULL;
    }

    uint16_t index = ctx->score->event_count++;
    music_event_t *event = &ctx->score->events[index];
    memset(event, 0, sizeof(*event));
    event->kind = kind;
    event->part_index = (uint16_t)ctx->current_part;
    event->measure_index = (uint16_t)ctx->current_measure;
    event->onset_divisions = onset;
    measure->event_count++;
    return event;
}

static int step_from_text(const char *text)
{
    switch (text && text[0] ? (char)toupper((unsigned char)text[0]) : 'C') {
    case 'D': return 1;
    case 'E': return 2;
    case 'F': return 3;
    case 'G': return 4;
    case 'A': return 5;
    case 'B': return 6;
    default: return 0;
    }
}

static music_accidental_t accidental_from_text(const char *text)
{
    if (!text) return MUSIC_ACCIDENTAL_NONE;
    if (strcmp(text, "flat") == 0) return MUSIC_ACCIDENTAL_FLAT;
    if (strcmp(text, "natural") == 0) return MUSIC_ACCIDENTAL_NATURAL;
    if (strcmp(text, "sharp") == 0) return MUSIC_ACCIDENTAL_SHARP;
    if (strcmp(text, "double-sharp") == 0 || strcmp(text, "sharp-sharp") == 0)
        return MUSIC_ACCIDENTAL_DOUBLE_SHARP;
    if (strcmp(text, "flat-flat") == 0 || strcmp(text, "double-flat") == 0)
        return MUSIC_ACCIDENTAL_DOUBLE_FLAT;
    return MUSIC_ACCIDENTAL_NONE;
}

static music_beam_state_t beam_from_text(const char *text)
{
    if (!text) return MUSIC_BEAM_NONE;
    if (strcmp(text, "begin") == 0) return MUSIC_BEAM_BEGIN;
    if (strcmp(text, "continue") == 0) return MUSIC_BEAM_CONTINUE;
    if (strcmp(text, "end") == 0) return MUSIC_BEAM_END;
    if (strcmp(text, "forward hook") == 0) return MUSIC_BEAM_FORWARD_HOOK;
    if (strcmp(text, "backward hook") == 0) return MUSIC_BEAM_BACKWARD_HOOK;
    return MUSIC_BEAM_NONE;
}

static music_duration_kind_t infer_duration(int32_t duration, int32_t divisions)
{
    if (duration <= 0 || divisions <= 0) return MUSIC_DURATION_UNKNOWN;
    if (duration >= divisions * 4) return MUSIC_DURATION_WHOLE;
    if (duration >= divisions * 2) return MUSIC_DURATION_HALF;
    if (duration >= divisions) return MUSIC_DURATION_QUARTER;
    if (duration * 2 >= divisions) return MUSIC_DURATION_EIGHTH;
    if (duration * 4 >= divisions) return MUSIC_DURATION_16TH;
    return MUSIC_DURATION_32ND;
}

static void commit_note(parser_ctx_t *ctx)
{
    music_measure_t *measure = current_measure(ctx);
    if (!measure) return;

    int32_t onset = ctx->note.chord ? ctx->last_note_onset : ctx->cursor;
    music_event_kind_t kind = ctx->note_is_rest ? MUSIC_EVENT_REST : MUSIC_EVENT_NOTE;
    music_event_t *event = append_event(ctx, kind, onset);
    if (!event) return;

    if (ctx->note.type == MUSIC_DURATION_UNKNOWN) {
        ctx->note.type = infer_duration(ctx->note.duration_divisions, ctx->divisions);
    }

    if (ctx->note_is_rest) {
        event->data.rest.type = ctx->note.type;
        event->data.rest.duration_divisions = ctx->note.duration_divisions;
        event->data.rest.dots = ctx->note.dots;
        event->data.rest.voice = ctx->note.voice;
        event->data.rest.staff = ctx->note.staff;
    } else {
        event->data.note = ctx->note;
    }

    if (!ctx->note.chord) {
        ctx->last_note_onset = onset;
        if (!ctx->note.grace) ctx->cursor += ctx->note.duration_divisions;
    }
    int32_t end = onset + (ctx->note.grace ? 0 : ctx->note.duration_divisions);
    if (end > ctx->max_cursor) ctx->max_cursor = end;
}

static void commit_clef(parser_ctx_t *ctx)
{
    uint8_t staff = ctx->attribute_staff;
    if (staff < 1) staff = 1;
    if (staff > MUSIC_MAX_STAVES) staff = MUSIC_MAX_STAVES;
    ctx->clefs[staff - 1] = ctx->pending_clef;
    music_measure_t *measure = current_measure(ctx);
    if (!measure) return;
    if (ctx->cursor == 0) {
        measure->clefs[staff - 1] = ctx->pending_clef;
    } else {
        music_event_t *event = append_event(ctx, MUSIC_EVENT_CLEF, ctx->cursor);
        if (event) {
            event->data.clef.value = ctx->pending_clef;
            event->data.clef.staff = staff;
        }
    }
}

static void commit_key(parser_ctx_t *ctx)
{
    ctx->key = ctx->pending_key;
    music_measure_t *measure = current_measure(ctx);
    if (!measure) return;
    if (ctx->cursor == 0) {
        measure->key = ctx->pending_key;
    } else {
        music_event_t *event = append_event(ctx, MUSIC_EVENT_KEY_SIGNATURE, ctx->cursor);
        if (event) {
            event->data.key.value = ctx->pending_key;
            event->data.key.staff = ctx->attribute_staff ? ctx->attribute_staff : 1;
        }
    }
}

static void commit_time(parser_ctx_t *ctx)
{
    ctx->time = ctx->pending_time;
    music_measure_t *measure = current_measure(ctx);
    if (!measure) return;
    if (ctx->cursor == 0) {
        measure->time = ctx->pending_time;
    } else {
        music_event_t *event = append_event(ctx, MUSIC_EVENT_TIME_SIGNATURE, ctx->cursor);
        if (event) {
            event->data.time.value = ctx->pending_time;
            event->data.time.staff = ctx->attribute_staff ? ctx->attribute_staff : 1;
        }
    }
}

static void XMLCALL start_element(void *user_data, const XML_Char *raw_name,
                                  const XML_Char **attrs)
{
    parser_ctx_t *ctx = (parser_ctx_t *)user_data;
    const char *name = local_name(raw_name);
    reset_text(ctx);

    if (strcmp(name, "score-timewise") == 0) {
        fail(ctx, "score-timewise is not supported; use score-partwise");
    } else if (strcmp(name, "part") == 0) {
        if (ctx->score->part_count >= MUSIC_MAX_PARTS) {
            fail(ctx, "MusicXML exceeds MUSIC_MAX_PARTS");
            return;
        }
        ctx->current_part = ctx->score->part_count++;
        music_part_t *part = &ctx->score->parts[ctx->current_part];
        memset(part, 0, sizeof(*part));
        copy_string(part->id, sizeof(part->id), attribute_value(attrs, "id"));
        part->measure_start = ctx->score->measure_count;
        part->staff_count = 1;

        ctx->divisions = 1;
        ctx->staff_count = 1;
        ctx->key = (music_key_signature_t){0};
        ctx->time = (music_time_signature_t){4, 4, MUSIC_TIME_NUMERIC};
        ctx->clefs[0] = (music_clef_t){MUSIC_CLEF_TREBLE, 2, 0};
        ctx->clefs[1] = (music_clef_t){MUSIC_CLEF_BASS, 4, 0};
    } else if (strcmp(name, "measure") == 0 && ctx->current_part >= 0) {
        if (ctx->score->measure_count >= MUSIC_MAX_MEASURES) {
            fail(ctx, "MusicXML exceeds MUSIC_MAX_MEASURES");
            return;
        }
        ctx->current_measure = ctx->score->measure_count++;
        music_measure_t *measure = &ctx->score->measures[ctx->current_measure];
        memset(measure, 0, sizeof(*measure));
        copy_string(measure->number, sizeof(measure->number),
                    attribute_value(attrs, "number"));
        measure->part_index = (uint16_t)ctx->current_part;
        measure->event_start = ctx->score->event_count;
        measure->divisions = ctx->divisions;
        measure->staff_count = ctx->staff_count;
        measure->clefs[0] = ctx->clefs[0];
        measure->clefs[1] = ctx->clefs[1];
        measure->key = ctx->key;
        measure->time = ctx->time;
        measure->right_barline = MUSIC_BARLINE_SINGLE;
        ctx->score->parts[ctx->current_part].measure_count++;
        ctx->cursor = 0;
        ctx->max_cursor = 0;
        ctx->last_note_onset = 0;
    } else if (strcmp(name, "attributes") == 0) {
        ctx->in_attributes = true;
    } else if (strcmp(name, "key") == 0 && ctx->in_attributes) {
        ctx->in_key = true;
        ctx->pending_key = ctx->key;
        ctx->pending_key.cancel_fifths = 0;
        ctx->attribute_staff = (uint8_t)atoi(attribute_value(attrs, "number") ?: "1");
    } else if (strcmp(name, "time") == 0 && ctx->in_attributes) {
        ctx->in_time = true;
        ctx->pending_time = ctx->time;
        const char *symbol = attribute_value(attrs, "symbol");
        ctx->pending_time.symbol = symbol && strcmp(symbol, "common") == 0 ? MUSIC_TIME_COMMON :
                                   symbol && strcmp(symbol, "cut") == 0 ? MUSIC_TIME_CUT :
                                   MUSIC_TIME_NUMERIC;
        ctx->attribute_staff = (uint8_t)atoi(attribute_value(attrs, "number") ?: "1");
    } else if (strcmp(name, "clef") == 0 && ctx->in_attributes) {
        ctx->in_clef = true;
        ctx->attribute_staff = (uint8_t)atoi(attribute_value(attrs, "number") ?: "1");
        uint8_t index = ctx->attribute_staff > 0 ? ctx->attribute_staff - 1 : 0;
        if (index >= MUSIC_MAX_STAVES) index = MUSIC_MAX_STAVES - 1;
        ctx->pending_clef = ctx->clefs[index];
    } else if (strcmp(name, "note") == 0) {
        ctx->in_note = true;
        memset(&ctx->note, 0, sizeof(ctx->note));
        ctx->note.voice = 1;
        ctx->note.staff = 1;
        ctx->note.stem = MUSIC_STEM_AUTO;
        ctx->note_is_rest = false;
        ctx->beam_number = 1;
    } else if (strcmp(name, "pitch") == 0 && ctx->in_note) {
        ctx->in_pitch = true;
    } else if (strcmp(name, "chord") == 0 && ctx->in_note) {
        ctx->note.chord = true;
    } else if (strcmp(name, "rest") == 0 && ctx->in_note) {
        ctx->note_is_rest = true;
    } else if (strcmp(name, "grace") == 0 && ctx->in_note) {
        ctx->note.grace = true;
    } else if (strcmp(name, "dot") == 0 && ctx->in_note) {
        if (ctx->note.dots < 3) ctx->note.dots++;
    } else if ((strcmp(name, "tie") == 0 || strcmp(name, "tied") == 0) && ctx->in_note) {
        const char *type = attribute_value(attrs, "type");
        if (type && strcmp(type, "start") == 0) ctx->note.tie_flags |= MUSIC_TIE_START;
        if (type && strcmp(type, "stop") == 0) ctx->note.tie_flags |= MUSIC_TIE_STOP;
    } else if (strcmp(name, "slur") == 0 && ctx->in_note) {
        const char *type = attribute_value(attrs, "type");
        int number = atoi(attribute_value(attrs, "number") ?: "1");
        if (number < 1) number = 1;
        if (number > 255) number = 255;
        if (type && strcmp(type, "start") == 0) ctx->note.slur_start = (uint8_t)number;
        if (type && strcmp(type, "stop") == 0) ctx->note.slur_stop = (uint8_t)number;
    } else if (strcmp(name, "articulations") == 0 && ctx->in_note) {
        ctx->in_articulations = true;
    } else if (ctx->in_articulations && strcmp(name, "staccato") == 0) {
        ctx->note.articulations |= MUSIC_ARTIC_STACCATO;
    } else if (ctx->in_articulations && strcmp(name, "accent") == 0) {
        ctx->note.articulations |= MUSIC_ARTIC_ACCENT;
    } else if (ctx->in_articulations && strcmp(name, "tenuto") == 0) {
        ctx->note.articulations |= MUSIC_ARTIC_TENUTO;
    } else if (strcmp(name, "fermata") == 0 && ctx->in_note) {
        ctx->note.articulations |= MUSIC_ARTIC_FERMATA;
    } else if (strcmp(name, "beam") == 0 && ctx->in_note) {
        ctx->beam_number = atoi(attribute_value(attrs, "number") ?: "1");
    } else if (strcmp(name, "backup") == 0) {
        ctx->in_backup = true;
        ctx->movement_duration = 0;
    } else if (strcmp(name, "forward") == 0) {
        ctx->in_forward = true;
        ctx->movement_duration = 0;
    } else if (strcmp(name, "barline") == 0) {
        ctx->in_barline = true;
        ctx->pending_barline = MUSIC_BARLINE_SINGLE;
    } else if (strcmp(name, "repeat") == 0 && ctx->in_barline) {
        const char *direction = attribute_value(attrs, "direction");
        if (direction && strcmp(direction, "forward") == 0)
            ctx->pending_barline = MUSIC_BARLINE_REPEAT_START;
        else if (direction && strcmp(direction, "backward") == 0)
            ctx->pending_barline = MUSIC_BARLINE_REPEAT_END;
    }
}

static void XMLCALL character_data(void *user_data, const XML_Char *data, int len)
{
    parser_ctx_t *ctx = (parser_ctx_t *)user_data;
    if (len <= 0 || ctx->text_len >= sizeof(ctx->text) - 1) return;
    size_t available = sizeof(ctx->text) - 1 - ctx->text_len;
    size_t copy_len = (size_t)len < available ? (size_t)len : available;
    memcpy(ctx->text + ctx->text_len, data, copy_len);
    ctx->text_len += copy_len;
    ctx->text[ctx->text_len] = '\0';
}

static void XMLCALL end_element(void *user_data, const XML_Char *raw_name)
{
    parser_ctx_t *ctx = (parser_ctx_t *)user_data;
    const char *name = local_name(raw_name);
    char *value = trim(ctx->text);

    if (strcmp(name, "movement-title") == 0 || strcmp(name, "work-title") == 0) {
        if (ctx->score->title[0] == '\0' && value[0])
            copy_string(ctx->score->title, sizeof(ctx->score->title), value);
    } else if (strcmp(name, "divisions") == 0 && ctx->in_attributes) {
        int v = atoi(value);
        if (v > 0) {
            ctx->divisions = v;
            music_measure_t *measure = current_measure(ctx);
            if (measure) measure->divisions = v;
        }
    } else if (strcmp(name, "staves") == 0 && ctx->in_attributes) {
        int v = atoi(value);
        if (v < 1) v = 1;
        if (v > MUSIC_MAX_STAVES) v = MUSIC_MAX_STAVES;
        ctx->staff_count = (uint8_t)v;
        music_measure_t *measure = current_measure(ctx);
        if (measure) measure->staff_count = (uint8_t)v;
        if (ctx->current_part >= 0) ctx->score->parts[ctx->current_part].staff_count = (uint8_t)v;
    } else if (strcmp(name, "fifths") == 0 && ctx->in_key) {
        int v = atoi(value);
        if (v < -7) v = -7;
        if (v > 7) v = 7;
        ctx->pending_key.fifths = (int8_t)v;
    } else if (strcmp(name, "cancel") == 0 && ctx->in_key) {
        int v = atoi(value);
        if (v < -7) v = -7;
        if (v > 7) v = 7;
        ctx->pending_key.cancel_fifths = (int8_t)v;
    } else if (strcmp(name, "mode") == 0 && ctx->in_key) {
        ctx->pending_key.minor = strcmp(value, "minor") == 0;
    } else if (strcmp(name, "beats") == 0 && ctx->in_time) {
        int v = atoi(value);
        if (v > 0 && v <= 255) ctx->pending_time.beats = (uint8_t)v;
    } else if (strcmp(name, "beat-type") == 0 && ctx->in_time) {
        int v = atoi(value);
        if (v > 0 && v <= 255) ctx->pending_time.beat_type = (uint8_t)v;
    } else if (strcmp(name, "sign") == 0 && ctx->in_clef) {
        ctx->pending_clef.kind = value[0] == 'F' ? MUSIC_CLEF_BASS :
                                 value[0] == 'C' ? MUSIC_CLEF_ALTO :
                                 MUSIC_CLEF_TREBLE;
    } else if (strcmp(name, "line") == 0 && ctx->in_clef) {
        ctx->pending_clef.line = (int8_t)atoi(value);
    } else if (strcmp(name, "clef-octave-change") == 0 && ctx->in_clef) {
        ctx->pending_clef.octave_change = (int8_t)atoi(value);
    } else if (strcmp(name, "step") == 0 && ctx->in_pitch) {
        ctx->note.pitch.step = (uint8_t)step_from_text(value);
    } else if (strcmp(name, "alter") == 0 && ctx->in_pitch) {
        ctx->note.pitch.alter = (int8_t)atoi(value);
    } else if (strcmp(name, "octave") == 0 && ctx->in_pitch) {
        ctx->note.pitch.octave = (int8_t)atoi(value);
    } else if (strcmp(name, "duration") == 0) {
        int32_t duration = (int32_t)strtol(value, NULL, 10);
        if (ctx->in_note) ctx->note.duration_divisions = duration;
        else if (ctx->in_backup || ctx->in_forward) ctx->movement_duration = duration;
    } else if (strcmp(name, "voice") == 0 && ctx->in_note) {
        int v = atoi(value);
        ctx->note.voice = (uint8_t)(v > 0 && v <= 255 ? v : 1);
    } else if (strcmp(name, "type") == 0 && ctx->in_note) {
        ctx->note.type = music_duration_from_name(value);
    } else if (strcmp(name, "staff") == 0 && ctx->in_note) {
        int v = atoi(value);
        ctx->note.staff = (uint8_t)(v > 0 && v <= MUSIC_MAX_STAVES ? v : 1);
    } else if (strcmp(name, "stem") == 0 && ctx->in_note) {
        ctx->note.stem = strcmp(value, "up") == 0 ? MUSIC_STEM_UP :
                         strcmp(value, "down") == 0 ? MUSIC_STEM_DOWN :
                         strcmp(value, "none") == 0 ? MUSIC_STEM_NONE : MUSIC_STEM_AUTO;
    } else if (strcmp(name, "accidental") == 0 && ctx->in_note) {
        ctx->note.accidental = accidental_from_text(value);
    } else if (strcmp(name, "beam") == 0 && ctx->in_note) {
        if (ctx->beam_number >= 1 && ctx->beam_number <= MUSIC_MAX_BEAM_LEVELS)
            ctx->note.beams[ctx->beam_number - 1] = beam_from_text(value);
    } else if (strcmp(name, "bar-style") == 0 && ctx->in_barline) {
        if (strcmp(value, "light-light") == 0) ctx->pending_barline = MUSIC_BARLINE_DOUBLE;
        else if (strcmp(value, "light-heavy") == 0) ctx->pending_barline = MUSIC_BARLINE_FINAL;
    } else if (strcmp(name, "pitch") == 0) {
        ctx->in_pitch = false;
    } else if (strcmp(name, "articulations") == 0) {
        ctx->in_articulations = false;
    } else if (strcmp(name, "note") == 0) {
        commit_note(ctx);
        ctx->in_note = false;
    } else if (strcmp(name, "backup") == 0) {
        ctx->cursor -= ctx->movement_duration;
        if (ctx->cursor < 0) ctx->cursor = 0;
        ctx->in_backup = false;
    } else if (strcmp(name, "forward") == 0) {
        ctx->cursor += ctx->movement_duration;
        if (ctx->cursor > ctx->max_cursor) ctx->max_cursor = ctx->cursor;
        ctx->in_forward = false;
    } else if (strcmp(name, "clef") == 0 && ctx->in_clef) {
        commit_clef(ctx);
        ctx->in_clef = false;
    } else if (strcmp(name, "key") == 0 && ctx->in_key) {
        commit_key(ctx);
        ctx->in_key = false;
    } else if (strcmp(name, "time") == 0 && ctx->in_time) {
        commit_time(ctx);
        ctx->in_time = false;
    } else if (strcmp(name, "attributes") == 0) {
        ctx->in_attributes = false;
    } else if (strcmp(name, "barline") == 0) {
        music_measure_t *measure = current_measure(ctx);
        if (measure) measure->right_barline = ctx->pending_barline;
        ctx->in_barline = false;
    } else if (strcmp(name, "measure") == 0) {
        music_measure_t *measure = current_measure(ctx);
        if (measure) measure->duration_divisions = ctx->max_cursor;
        ctx->current_measure = -1;
    } else if (strcmp(name, "part") == 0) {
        ctx->current_part = -1;
    }

    reset_text(ctx);
}

bool musicxml_parse_buffer(const char *xml, size_t length,
                           music_score_t *score,
                           musicxml_error_t *error)
{
    if (!xml || length == 0 || !score) return false;
    music_score_init(score);
    if (error) memset(error, 0, sizeof(*error));

    XML_Parser parser = XML_ParserCreate(NULL);
    if (!parser) {
        if (error) copy_string(error->message, sizeof(error->message),
                               "Unable to create Expat parser");
        return false;
    }

    parser_ctx_t ctx = {
        .score = score,
        .error = error,
        .parser = parser,
        .current_part = -1,
        .current_measure = -1,
        .divisions = 1,
        .staff_count = 1,
        .time = {4, 4, MUSIC_TIME_NUMERIC},
    };
    ctx.clefs[0] = (music_clef_t){MUSIC_CLEF_TREBLE, 2, 0};
    ctx.clefs[1] = (music_clef_t){MUSIC_CLEF_BASS, 4, 0};

    XML_SetUserData(parser, &ctx);
    XML_SetElementHandler(parser, start_element, end_element);
    XML_SetCharacterDataHandler(parser, character_data);

    enum XML_Status status = XML_Parse(parser, xml, (int)length, XML_TRUE);
    if (status == XML_STATUS_ERROR && !ctx.failed && error) {
        error->line = XML_GetCurrentLineNumber(parser);
        error->column = XML_GetCurrentColumnNumber(parser);
        copy_string(error->message, sizeof(error->message),
                    XML_ErrorString(XML_GetErrorCode(parser)));
    }
    XML_ParserFree(parser);

    if (status == XML_STATUS_ERROR || ctx.failed) return false;
    if (score->part_count == 0 || score->measure_count == 0) {
        if (error) copy_string(error->message, sizeof(error->message),
                               "MusicXML contains no score-partwise measures");
        return false;
    }
    return true;
}
