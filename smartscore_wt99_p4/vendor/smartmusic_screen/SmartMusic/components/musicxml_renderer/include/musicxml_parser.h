#ifndef MUSICXML_PARSER_H
#define MUSICXML_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include "music_model.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t line;
    size_t column;
    char message[128];
} musicxml_error_t;

bool musicxml_parse_buffer(const char *xml, size_t length,
                           music_score_t *score,
                           musicxml_error_t *error);

#ifdef __cplusplus
}
#endif

#endif
