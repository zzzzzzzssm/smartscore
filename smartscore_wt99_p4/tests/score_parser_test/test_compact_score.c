#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "compact_score.h"
#include "dashscope_omr_response.h"

static unsigned tests_run;
static unsigned tests_failed;

#define CHECK(condition)                                                        \
    do {                                                                        \
        ++tests_run;                                                            \
        if (!(condition)) {                                                     \
            ++tests_failed;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,           \
                    #condition);                                                \
        }                                                                       \
    } while (0)

static const char numbered_json[] =
    "{\"v\":1,\"kind\":\"n\","
    "\"meta\":{\"nkey\":\"C\",\"ks\":null,\"ts\":[4,4],"
    "\"bpm\":84,\"ppq\":24},"
    "\"sys\":[{\"bars\":[{\"bf\":0,\"volta\":[],\"e\":["
    "[0,5,0,24,0,0,\"你\"],[24,3,0,12,0,0,\"可\"],"
    "[36,5,0,12,0,0,\"知\"],[48,0,0,48,0,0,\"\"]]}]}]}";

static const char staff_json[] =
    "{\"v\":1,\"kind\":\"s\","
    "\"meta\":{\"nkey\":null,\"ks\":-1,\"ts\":[4,4],"
    "\"bpm\":96,\"ppq\":24},"
    "\"sys\":[{\"staves\":["
    "{\"clef\":\"G\",\"bars\":[{\"bf\":0,\"volta\":[],\"e\":["
    "[0,[62],24,1,0,\"\"],[24,[69],12,1,0,\"\"],"
    "[36,[67],12,1,0,\"\"],[48,[69],48,1,1,\"\"]]}]},"
    "{\"clef\":\"F\",\"bars\":[{\"bf\":0,\"volta\":[],\"e\":["
    "[0,[46],24,1,0,\"\"],[24,[62,53,58],72,1,1,\"\"]]}]}]}]}";

static char *make_completion(const char *content, const char *finish_reason)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *choices = cJSON_AddArrayToObject(root, "choices");
    cJSON *choice = cJSON_CreateObject();
    cJSON_AddItemToArray(choices, choice);
    cJSON_AddStringToObject(choice, "finish_reason", finish_reason);
    cJSON *message = cJSON_AddObjectToObject(choice, "message");
    cJSON_AddStringToObject(message, "content", content);
    cJSON *usage = cJSON_AddObjectToObject(root, "usage");
    cJSON_AddNumberToObject(usage, "prompt_tokens", 100);
    cJSON_AddNumberToObject(usage, "completion_tokens", 20);
    cJSON_AddNumberToObject(usage, "total_tokens", 120);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static void test_numbered_score(void)
{
    compact_score_document_t *score = NULL;
    compact_score_error_t error = {0};
    CHECK(compact_score_parse(numbered_json, strlen(numbered_json), &score,
                              &error) == COMPACT_SCORE_OK);
    CHECK(score != NULL);
    if (score == NULL) return;
    CHECK(score->kind == COMPACT_SCORE_KIND_NUMBERED);
    CHECK(score->event_count == 4U);
    CHECK(score->sounding_note_count == 3U);
    CHECK(score->measure_ticks == 96U);
    CHECK(strcmp(score->systems[0].bars[0].events[0].lyric, "你") == 0);

    compact_score_playback_t playback = {0};
    CHECK(compact_score_build_playback(score, 120, &playback, &error) ==
          COMPACT_SCORE_OK);
    CHECK(playback.note_count == 3U);
    CHECK(playback.notes[0].midi == 67U);
    compact_score_playback_free(&playback);
    compact_score_free(score);
}

static void test_staff_grand_score(void)
{
    compact_score_document_t *score = NULL;
    compact_score_error_t error = {0};
    CHECK(compact_score_parse(staff_json, strlen(staff_json), &score, &error) ==
          COMPACT_SCORE_OK);
    CHECK(score != NULL);
    if (score == NULL) return;
    CHECK(score->kind == COMPACT_SCORE_KIND_STAFF);
    CHECK(score->systems[0].staff_count == 2U);
    CHECK(score->event_count == 6U);
    CHECK(score->sounding_note_count == 8U);
    const compact_score_event_t *chord =
        &score->systems[0].staves[1].bars[0].events[1];
    CHECK(chord->midi_count == 3U);
    CHECK(chord->midi[0] == 53U && chord->midi[1] == 58U &&
          chord->midi[2] == 62U);

    compact_score_playback_t playback = {0};
    CHECK(compact_score_build_playback(score, 120, &playback, &error) ==
          COMPACT_SCORE_OK);
    CHECK(playback.note_count == 8U);
    compact_score_playback_free(&playback);
    compact_score_free(score);
}

static void test_sorting_and_validation(void)
{
    const char unordered[] =
        "{\"v\":1,\"kind\":\"n\",\"meta\":{\"nkey\":\"C\","
        "\"ks\":null,\"ts\":[4,4],\"bpm\":120,\"ppq\":24},"
        "\"sys\":[{\"bars\":[{\"bf\":0,\"volta\":[],\"e\":["
        "[24,2,0,24,0,0,\"\"],[0,1,0,24,0,0,\"\"]]}]}]}";
    compact_score_document_t *score = NULL;
    compact_score_error_t error = {0};
    CHECK(compact_score_parse(unordered, strlen(unordered), &score, &error) ==
          COMPACT_SCORE_OK);
    CHECK(score != NULL && score->systems[0].bars[0].events[0].start == 0U);
    CHECK(score != NULL && score->reordered);
    compact_score_free(score);

    const char overflow[] =
        "{\"v\":1,\"kind\":\"n\",\"meta\":{\"nkey\":\"C\","
        "\"ks\":null,\"ts\":[4,4],\"bpm\":120,\"ppq\":24},"
        "\"sys\":[{\"bars\":[{\"bf\":0,\"volta\":[],"
        "\"e\":[[90,1,0,12,0,0,\"\"]]}]}]}";
    score = NULL;
    CHECK(compact_score_parse(overflow, strlen(overflow), &score, &error) ==
          COMPACT_SCORE_ERR_MEASURE_OVERFLOW);
    CHECK(score == NULL);

    const char bad_length[] =
        "{\"v\":1,\"kind\":\"n\",\"meta\":{\"nkey\":\"C\","
        "\"ks\":null,\"ts\":null,\"bpm\":null,\"ppq\":24},"
        "\"sys\":[{\"bars\":[{\"bf\":0,\"volta\":[],"
        "\"e\":[[0,1,0,24,0,0]]}]}]}";
    CHECK(compact_score_parse(bad_length, strlen(bad_length), &score, &error) ==
          COMPACT_SCORE_ERR_EVENT);

    const char bad_flags[] =
        "{\"v\":1,\"kind\":\"n\",\"meta\":{\"nkey\":\"C\","
        "\"ks\":null,\"ts\":null,\"bpm\":null,\"ppq\":24},"
        "\"sys\":[{\"bars\":[{\"bf\":0,\"volta\":[],"
        "\"e\":[[0,1,0,24,0,16,\"\"]]}]}]}";
    CHECK(compact_score_parse(bad_flags, strlen(bad_flags), &score, &error) ==
          COMPACT_SCORE_ERR_FLAGS);

    const char slide_flags[] =
        "{\"v\":1,\"kind\":\"n\",\"meta\":{\"nkey\":\"C\","
        "\"ks\":null,\"ts\":null,\"bpm\":null,\"ppq\":24},"
        "\"sys\":[{\"bars\":[{\"bf\":0,\"volta\":[],"
        "\"e\":[[0,1,0,24,0,8,\"\"]]}]}]}";
    CHECK(compact_score_parse(slide_flags, strlen(slide_flags), &score,
                              &error) == COMPACT_SCORE_OK);
    CHECK(score != NULL && score->systems[0].bars[0].events[0].flags == 8U);
    compact_score_playback_t slide_playback = {0};
    CHECK(compact_score_build_playback(score, 120, &slide_playback,
                                       &error) == COMPACT_SCORE_OK);
    CHECK(slide_playback.note_count == 1U &&
          slide_playback.notes[0].flags == 8U);
    compact_score_playback_free(&slide_playback);
    compact_score_free(score);
}

static void test_completion_layers(void)
{
    char *outer = make_completion(numbered_json, "stop");
    dashscope_omr_parsed_response_t parsed = {0};
    CHECK(dashscope_omr_parse_completion(outer, strlen(outer), &parsed) ==
          DASHSCOPE_OMR_OK);
    CHECK(parsed.score != NULL && parsed.score->event_count == 4U);
    CHECK(parsed.usage.present);
    CHECK(parsed.usage.input_tokens == 100U);
    CHECK(parsed.usage.output_tokens == 20U);
    dashscope_omr_parsed_response_free(&parsed);
    cJSON_free(outer);

    outer = make_completion(numbered_json, "length");
    CHECK(dashscope_omr_parse_completion(outer, strlen(outer), &parsed) ==
          DASHSCOPE_OMR_ERR_FINISH_LENGTH);
    cJSON_free(outer);

    CHECK(dashscope_omr_parse_completion("{bad", 4U, &parsed) ==
          DASHSCOPE_OMR_ERR_OUTER_JSON);
    CHECK(dashscope_omr_parse_completion(NULL, 0U, &parsed) ==
          DASHSCOPE_OMR_ERR_OUTER_JSON);

    outer = make_completion("{", "stop");
    CHECK(dashscope_omr_parse_completion(outer, strlen(outer), &parsed) ==
          DASHSCOPE_OMR_ERR_INNER_JSON);
    cJSON_free(outer);

    const char schema_error[] =
        "{\"v\":1,\"kind\":\"n\",\"meta\":{\"nkey\":\"C\","
        "\"ks\":null,\"ts\":null,\"bpm\":null,\"ppq\":24},"
        "\"sys\":[{\"bars\":[{\"bf\":0,\"volta\":[],"
        "\"e\":[[0,8,0,24,0,0,\"\"]]}]}]}";
    outer = make_completion(schema_error, "stop");
    CHECK(dashscope_omr_parse_completion(outer, strlen(outer), &parsed) ==
          DASHSCOPE_OMR_ERR_SCORE_SCHEMA);
    CHECK(parsed.score_error.code == COMPACT_SCORE_ERR_RANGE);
    cJSON_free(outer);
}

static void test_http_policy(void)
{
    CHECK(dashscope_omr_error_from_http_status(200) == DASHSCOPE_OMR_OK);
    CHECK(dashscope_omr_error_from_http_status(401) ==
          DASHSCOPE_OMR_ERR_HTTP_UNAUTHORIZED);
    CHECK(!dashscope_omr_should_retry(DASHSCOPE_OMR_ERR_HTTP_UNAUTHORIZED,
                                     401));
    CHECK(dashscope_omr_error_from_http_status(429) ==
          DASHSCOPE_OMR_ERR_HTTP_RATE_LIMIT);
    CHECK(dashscope_omr_should_retry(DASHSCOPE_OMR_ERR_HTTP_RATE_LIMIT, 429));
    CHECK(dashscope_omr_should_retry(DASHSCOPE_OMR_ERR_HTTP_SERVER, 503));
    CHECK(dashscope_omr_should_retry(DASHSCOPE_OMR_ERR_NETWORK, 0));
    CHECK(dashscope_omr_should_retry(DASHSCOPE_OMR_ERR_TIMEOUT, 0));
    CHECK(!dashscope_omr_should_retry(DASHSCOPE_OMR_ERR_HTTP_STATUS, 400));
}

int main(void)
{
    test_numbered_score();
    test_staff_grand_score();
    test_sorting_and_validation();
    test_completion_layers();
    test_http_policy();
    printf("compact score tests: %u checks, %u failures\n", tests_run,
           tests_failed);
    return tests_failed == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}
