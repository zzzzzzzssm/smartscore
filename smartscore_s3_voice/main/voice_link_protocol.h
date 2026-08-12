#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_LINK_MAGIC_0 0xA5U
#define VOICE_LINK_MAGIC_1 0x5AU
#define VOICE_LINK_VERSION 0x02U
#define VOICE_LINK_MAX_PAYLOAD_BYTES 192U
#define VOICE_LINK_WIRE_OVERHEAD_BYTES 13U

typedef enum {
    VOICE_LINK_PACKET_AUDIO = 1,
    VOICE_LINK_PACKET_ACK = 2,
    VOICE_LINK_PACKET_NACK = 3,
} voice_link_packet_type_t;

typedef struct {
    uint8_t type;
    uint8_t flags;
    uint16_t sequence;
    uint16_t payload_length;
    uint8_t payload[VOICE_LINK_MAX_PAYLOAD_BYTES];
} voice_link_packet_t;

typedef enum {
    VOICE_LINK_PARSE_MORE = 0,
    VOICE_LINK_PARSE_COMPLETE,
    VOICE_LINK_PARSE_ERROR,
} voice_link_parse_result_t;

typedef struct {
    uint8_t header[9];
    size_t header_used;
    voice_link_packet_t packet;
    size_t payload_used;
    uint8_t payload_crc[2];
    size_t payload_crc_used;
} voice_link_parser_t;

uint16_t voice_link_crc16(const void *data, size_t length);

esp_err_t voice_link_encode_packet(uint8_t type,
                                   uint8_t flags,
                                   uint16_t sequence,
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
