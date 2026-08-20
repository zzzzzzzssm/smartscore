#include <string.h>

#include "s3_protocol.h"
#include "unity.h"

TEST_CASE("music protocol parses note_on", "[s3_bus]")
{
    const char *line =
        "{\"v\":1,\"type\":\"note_on\",\"seq\":7,\"sid\":2,"
        "\"ts_ms\":1234,\"midi\":69,\"velocity\":100,"
        "\"freq_hz\":440.0,\"confidence\":0.95}";
    s3_music_message_t message;
    TEST_ASSERT_EQUAL(
        S3_PROTOCOL_OK,
        s3_protocol_parse_music_line(line, strlen(line), &message));
    TEST_ASSERT_EQUAL(S3_MUSIC_MESSAGE_NOTE_ON, message.type);
    TEST_ASSERT_EQUAL_UINT32(7, message.seq);
    TEST_ASSERT_EQUAL_UINT32(2, message.sid);
    TEST_ASSERT_EQUAL_UINT32(1234, message.ts_ms);
    TEST_ASSERT_EQUAL_UINT8(69, message.midi);
    TEST_ASSERT_EQUAL_UINT8(100, message.velocity);
    TEST_ASSERT_TRUE(message.has_frequency);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 440.0f, message.frequency_hz);
    TEST_ASSERT_TRUE(message.has_confidence);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.95f, message.confidence);
}

TEST_CASE("music protocol parses note_off", "[s3_bus]")
{
    const char *line =
        "{\"v\":1,\"type\":\"note_off\",\"seq\":8,\"sid\":2,"
        "\"ts_ms\":1300,\"midi\":69,\"duration_ms\":66,"
        "\"reason\":\"silence\"}";
    s3_music_message_t message;
    TEST_ASSERT_EQUAL(
        S3_PROTOCOL_OK,
        s3_protocol_parse_music_line(line, strlen(line), &message));
    TEST_ASSERT_EQUAL(S3_MUSIC_MESSAGE_NOTE_OFF, message.type);
    TEST_ASSERT_EQUAL_UINT8(69, message.midi);
    TEST_ASSERT_EQUAL_UINT8(0, message.velocity);
    TEST_ASSERT_TRUE(message.has_duration);
    TEST_ASSERT_EQUAL_UINT32(66, message.duration_ms);
}

TEST_CASE("music protocol parses interval and chord poly frames", "[s3_bus]")
{
    const char *interval =
        "{\"v\":1,\"type\":\"poly\",\"kind\":\"interval\","
        "\"seq\":9,\"sid\":2,\"ts_ms\":1400,\"notes\":[60,67],"
        "\"confidence\":0.84}";
    s3_music_message_t message;
    TEST_ASSERT_EQUAL(
        S3_PROTOCOL_OK,
        s3_protocol_parse_music_line(interval, strlen(interval), &message));
    TEST_ASSERT_EQUAL(S3_MUSIC_MESSAGE_POLY, message.type);
    TEST_ASSERT_EQUAL(S3_PROTOCOL_POLY_INTERVAL, message.poly_kind);
    TEST_ASSERT_EQUAL_UINT8(2, message.note_count);
    TEST_ASSERT_EQUAL_UINT8(60, message.notes[0]);
    TEST_ASSERT_EQUAL_UINT8(67, message.notes[1]);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.84f, message.confidence);

    const char *chord =
        "{\"v\":1,\"type\":\"poly\",\"kind\":\"chord\","
        "\"seq\":10,\"sid\":2,\"ts_ms\":1500,\"name\":\"C:maj\","
        "\"notes\":[60,64,67],\"confidence\":0.91}";
    TEST_ASSERT_EQUAL(
        S3_PROTOCOL_OK,
        s3_protocol_parse_music_line(chord, strlen(chord), &message));
    TEST_ASSERT_EQUAL(S3_PROTOCOL_POLY_CHORD, message.poly_kind);
    TEST_ASSERT_EQUAL_UINT8(3, message.note_count);
    TEST_ASSERT_EQUAL_STRING("C:maj", message.poly_name);
}

TEST_CASE("music protocol rejects malformed poly notes", "[s3_bus]")
{
    const char *duplicate =
        "{\"v\":1,\"type\":\"poly\",\"kind\":\"interval\","
        "\"seq\":11,\"sid\":2,\"ts_ms\":1600,\"notes\":[60,60]}";
    s3_music_message_t message;
    TEST_ASSERT_EQUAL(
        S3_PROTOCOL_INVALID_FIELD,
        s3_protocol_parse_music_line(duplicate, strlen(duplicate), &message));
}

TEST_CASE("music protocol rejects missing and out of range fields", "[s3_bus]")
{
    const char *missing =
        "{\"v\":1,\"type\":\"note_on\",\"seq\":1,\"sid\":0,"
        "\"ts_ms\":10,\"midi\":60}";
    const char *range =
        "{\"v\":1,\"type\":\"note_on\",\"seq\":1,\"sid\":0,"
        "\"ts_ms\":10,\"midi\":128,\"velocity\":90}";
    s3_music_message_t message;
    TEST_ASSERT_EQUAL(
        S3_PROTOCOL_INVALID_FIELD,
        s3_protocol_parse_music_line(missing, strlen(missing), &message));
    TEST_ASSERT_EQUAL(
        S3_PROTOCOL_INVALID_FIELD,
        s3_protocol_parse_music_line(range, strlen(range), &message));
}

TEST_CASE("music protocol rejects unknown type and trailing garbage", "[s3_bus]")
{
    const char *unknown =
        "{\"v\":1,\"type\":\"raw_pcm\",\"seq\":1,\"sid\":0,\"ts_ms\":10}";
    const char *trailing =
        "{\"v\":1,\"type\":\"pong\",\"seq\":1,\"sid\":0,\"ts_ms\":10}x";
    s3_music_message_t message;
    TEST_ASSERT_EQUAL(
        S3_PROTOCOL_UNKNOWN_TYPE,
        s3_protocol_parse_music_line(unknown, strlen(unknown), &message));
    TEST_ASSERT_EQUAL(
        S3_PROTOCOL_INVALID_JSON,
        s3_protocol_parse_music_line(trailing, strlen(trailing), &message));
}

TEST_CASE("MusicLink v2 parses a sorted four-key snapshot", "[s3_bus]")
{
    const char *line =
        "{\"v\":2,\"type\":\"notes\",\"seq\":8,\"sid\":3,"
        "\"state_id\":5,\"ts_ms\":900,\"midis\":[36,48,60,96],"
        "\"velocities\":[80,81,82,83],"
        "\"confidences\":[0.9,0.8,0.7,0.6],"
        "\"set_confidence\":0.75,\"degraded_mic\":false,"
        "\"overflow\":false}";
    s3_music_message_t message;
    TEST_ASSERT_EQUAL(
        S3_PROTOCOL_OK,
        s3_protocol_parse_music_line(line, strlen(line), &message));
    TEST_ASSERT_EQUAL(S3_MUSIC_MESSAGE_NOTES, message.type);
    TEST_ASSERT_EQUAL_UINT32(2, message.version);
    TEST_ASSERT_EQUAL_UINT32(5, message.state_id);
    TEST_ASSERT_EQUAL_UINT8(4, message.note_set_count);
    TEST_ASSERT_EQUAL_UINT8(36, message.midis[0]);
    TEST_ASSERT_EQUAL_UINT8(96, message.midis[3]);
    TEST_ASSERT_FALSE(message.degraded_mic);
    TEST_ASSERT_FALSE(message.overflow);
}

TEST_CASE("MusicLink v2 parses an empty release snapshot", "[s3_bus]")
{
    const char *line =
        "{\"v\":2,\"type\":\"notes\",\"seq\":9,\"sid\":3,"
        "\"state_id\":6,\"ts_ms\":1000,\"midis\":[],"
        "\"velocities\":[],\"confidences\":[],"
        "\"set_confidence\":1.0,\"degraded_mic\":true}";
    s3_music_message_t message;
    TEST_ASSERT_EQUAL(
        S3_PROTOCOL_OK,
        s3_protocol_parse_music_line(line, strlen(line), &message));
    TEST_ASSERT_EQUAL_UINT8(0, message.note_set_count);
    TEST_ASSERT_TRUE(message.degraded_mic);
    TEST_ASSERT_FALSE(message.overflow);
}

TEST_CASE("MusicLink v2 rejects invalid note arrays", "[s3_bus]")
{
    const char *unsorted =
        "{\"v\":2,\"type\":\"notes\",\"seq\":1,\"sid\":1,"
        "\"state_id\":1,\"ts_ms\":1,\"midis\":[60,48],"
        "\"velocities\":[80,80],\"confidences\":[0.9,0.9],"
        "\"set_confidence\":0.9,\"degraded_mic\":false}";
    const char *mismatched =
        "{\"v\":2,\"type\":\"notes\",\"seq\":1,\"sid\":1,"
        "\"state_id\":1,\"ts_ms\":1,\"midis\":[48,60],"
        "\"velocities\":[80],\"confidences\":[0.9,0.9],"
        "\"set_confidence\":0.9,\"degraded_mic\":false}";
    const char *out_of_range =
        "{\"v\":2,\"type\":\"notes\",\"seq\":1,\"sid\":1,"
        "\"state_id\":1,\"ts_ms\":1,\"midis\":[35],"
        "\"velocities\":[80],\"confidences\":[0.9],"
        "\"set_confidence\":0.9,\"degraded_mic\":false}";
    s3_music_message_t message;
    TEST_ASSERT_EQUAL(
        S3_PROTOCOL_INVALID_FIELD,
        s3_protocol_parse_music_line(unsorted, strlen(unsorted), &message));
    TEST_ASSERT_EQUAL(
        S3_PROTOCOL_INVALID_FIELD,
        s3_protocol_parse_music_line(mismatched, strlen(mismatched),
                                     &message));
    TEST_ASSERT_EQUAL(
        S3_PROTOCOL_INVALID_FIELD,
        s3_protocol_parse_music_line(out_of_range, strlen(out_of_range),
                                     &message));
}

TEST_CASE("music protocol enforces message version", "[s3_bus]")
{
    const char *v1_notes =
        "{\"v\":1,\"type\":\"notes\",\"seq\":1,\"sid\":1,"
        "\"state_id\":1,\"ts_ms\":1,\"midis\":[],"
        "\"velocities\":[],\"confidences\":[],"
        "\"set_confidence\":1.0,\"degraded_mic\":false}";
    const char *v2_note_on =
        "{\"v\":2,\"type\":\"note_on\",\"seq\":1,\"sid\":1,"
        "\"ts_ms\":1,\"midi\":60,\"velocity\":80}";
    s3_music_message_t message;
    TEST_ASSERT_EQUAL(
        S3_PROTOCOL_INVALID_FIELD,
        s3_protocol_parse_music_line(v1_notes, strlen(v1_notes), &message));
    TEST_ASSERT_EQUAL(
        S3_PROTOCOL_INVALID_FIELD,
        s3_protocol_parse_music_line(v2_note_on, strlen(v2_note_on),
                                     &message));
}
