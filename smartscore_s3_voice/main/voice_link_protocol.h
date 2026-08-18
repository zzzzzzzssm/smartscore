#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_LINK_MAGIC_0 0xA5U
#define VOICE_LINK_MAGIC_1 0x5AU
#define VOICE_LINK_VERSION 0x03U
#define VOICE_LINK_MAX_PAYLOAD_BYTES 2048U
#define VOICE_LINK_WIRE_OVERHEAD_BYTES 15U
#define VOICE_LINK_PCM_PAYLOAD_BYTES 1536U

typedef enum {
    VOICE_MSG_WAKE = 1,
    VOICE_MSG_TIMEOUT = 2,
    VOICE_MSG_LOCAL_COMMAND = 3,
    VOICE_MSG_LOCAL_COMMAND_ACK = 4,
    VOICE_MSG_AI_STATE = 5,
    VOICE_MSG_AI_AUDIO_START = 6,
    VOICE_MSG_AI_AUDIO_PCM = 7,
    VOICE_MSG_AI_AUDIO_DONE = 8,
    VOICE_MSG_AI_TEXT = 9,
    VOICE_MSG_AI_ERROR = 10,
    VOICE_MSG_STOP = 11,
    VOICE_MSG_STOP_ACK = 12,
    VOICE_MSG_FLOW_OFF = 13,
    VOICE_MSG_FLOW_ON = 14,
    VOICE_MSG_WIFI_CREDENTIALS = 15,
    VOICE_MSG_WIFI_STATUS = 16,
    VOICE_MSG_AI_AUDIO_DRAINED = 17,
    VOICE_LINK_PACKET_AUDIO = 0x80,
    VOICE_LINK_PACKET_ACK = 0x81,
    VOICE_LINK_PACKET_NACK = 0x82,
} voice_link_packet_type_t;

typedef enum {
    VOICE_AI_STATE_IDLE = 0,
    VOICE_AI_STATE_LISTENING = 1,
    VOICE_AI_STATE_THINKING = 2,
    VOICE_AI_STATE_SPEAKING = 3,
    VOICE_AI_STATE_ERROR = 4,
} voice_ai_state_t;

typedef struct {
    uint8_t type;
    uint8_t flags;
    uint32_t sequence;
    uint16_t payload_length;
    uint8_t payload[VOICE_LINK_MAX_PAYLOAD_BYTES];
} voice_link_packet_t;

typedef enum {
    VOICE_LINK_PARSE_MORE = 0,
    VOICE_LINK_PARSE_COMPLETE,
    VOICE_LINK_PARSE_ERROR,
} voice_link_parse_result_t;

typedef struct {
    uint8_t header[11];
    size_t header_used;
    voice_link_packet_t packet;
    size_t payload_used;
    uint8_t payload_crc[2];
    size_t payload_crc_used;
} voice_link_parser_t;

uint16_t voice_link_crc16(const void *data, size_t length);

esp_err_t voice_link_encode_packet(uint8_t type,
                                   uint8_t flags,
                                   uint32_t sequence,
                                   const void *payload,
                                   uint16_t payload_length,
                                   uint8_t *output,
                                   size_t output_capacity,
                                   size_t *output_length);

void voice_link_parser_begin(voice_link_parser_t *parser);

voice_link_parse_result_t voice_link_parser_feed(
    voice_link_parser_t *parser,
    uint8_t byte,
    voice_link_packet_t *packet);

#ifdef __cplusplus
}
#endif
