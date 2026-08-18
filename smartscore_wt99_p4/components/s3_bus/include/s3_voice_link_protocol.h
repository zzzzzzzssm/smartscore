#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define S3_VOICE_LINK_MAGIC_0 0xA5U
#define S3_VOICE_LINK_MAGIC_1 0x5AU
#define S3_VOICE_LINK_VERSION 0x03U
#define S3_VOICE_LINK_MAX_PAYLOAD_BYTES 2048U
#define S3_VOICE_LINK_WIRE_OVERHEAD_BYTES 15U
#define S3_VOICE_LINK_PCM_PAYLOAD_BYTES 1536U

typedef enum {
    S3_VOICE_MSG_WAKE = 1,
    S3_VOICE_MSG_TIMEOUT = 2,
    S3_VOICE_MSG_LOCAL_COMMAND = 3,
    S3_VOICE_MSG_LOCAL_COMMAND_ACK = 4,
    S3_VOICE_MSG_AI_STATE = 5,
    S3_VOICE_MSG_AI_AUDIO_START = 6,
    S3_VOICE_MSG_AI_AUDIO_PCM = 7,
    S3_VOICE_MSG_AI_AUDIO_DONE = 8,
    S3_VOICE_MSG_AI_TEXT = 9,
    S3_VOICE_MSG_AI_ERROR = 10,
    S3_VOICE_MSG_STOP = 11,
    S3_VOICE_MSG_STOP_ACK = 12,
    S3_VOICE_MSG_FLOW_OFF = 13,
    S3_VOICE_MSG_FLOW_ON = 14,
    S3_VOICE_MSG_WIFI_CREDENTIALS = 15,
    S3_VOICE_MSG_WIFI_STATUS = 16,
    S3_VOICE_MSG_AI_AUDIO_DRAINED = 17,
    S3_VOICE_LINK_PACKET_AUDIO = 0x80,
    S3_VOICE_LINK_PACKET_ACK = 0x81,
    S3_VOICE_LINK_PACKET_NACK = 0x82,
} s3_voice_link_packet_type_t;

typedef enum {
    S3_VOICE_AI_STATE_IDLE = 0,
    S3_VOICE_AI_STATE_LISTENING = 1,
    S3_VOICE_AI_STATE_THINKING = 2,
    S3_VOICE_AI_STATE_SPEAKING = 3,
    S3_VOICE_AI_STATE_ERROR = 4,
} s3_voice_ai_state_t;

typedef struct {
    uint8_t type;
    uint8_t flags;
    uint32_t sequence;
    uint16_t payload_length;
    uint8_t payload[S3_VOICE_LINK_MAX_PAYLOAD_BYTES];
} s3_voice_link_packet_t;

typedef enum {
    S3_VOICE_LINK_PARSE_MORE = 0,
    S3_VOICE_LINK_PARSE_COMPLETE,
    S3_VOICE_LINK_PARSE_ERROR,
} s3_voice_link_parse_result_t;

typedef struct {
    uint8_t header[11];
    size_t header_used;
    s3_voice_link_packet_t packet;
    size_t payload_used;
    uint8_t payload_crc[2];
    size_t payload_crc_used;
} s3_voice_link_parser_t;

uint16_t s3_voice_link_crc16(const void *data, size_t length);
esp_err_t s3_voice_link_encode_packet(uint8_t type,
                                      uint8_t flags,
                                      uint32_t sequence,
                                      const void *payload,
                                      uint16_t payload_length,
                                      uint8_t *output,
                                      size_t output_capacity,
                                      size_t *output_length);
void s3_voice_link_parser_begin(s3_voice_link_parser_t *parser);
s3_voice_link_parse_result_t s3_voice_link_parser_feed(
    s3_voice_link_parser_t *parser,
    uint8_t byte,
    s3_voice_link_packet_t *packet);

#ifdef __cplusplus
}
#endif
