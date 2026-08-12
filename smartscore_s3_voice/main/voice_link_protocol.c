#include "voice_link_protocol.h"

#include <string.h>

static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}

static void write_u16_le(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value & 0xFFU);
    data[1] = (uint8_t)(value >> 8U);
}

uint16_t voice_link_crc16(const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint16_t crc = 0xFFFFU;
    for (size_t index = 0; index < length; ++index) {
        crc ^= (uint16_t)bytes[index] << 8U;
        for (unsigned bit = 0; bit < 8U; ++bit) {
            crc = (crc & 0x8000U) != 0U
                      ? (uint16_t)((crc << 1U) ^ 0x1021U)
                      : (uint16_t)(crc << 1U);
        }
    }
    return crc;
}

esp_err_t voice_link_encode_packet(uint8_t type,
                                   uint8_t flags,
                                   uint16_t sequence,
                                   const void *payload,
                                   uint16_t payload_length,
                                   uint8_t *output,
                                   size_t output_capacity,
                                   size_t *output_length)
{
    if (output == NULL || output_length == NULL ||
        (payload_length > 0U && payload == NULL) ||
        payload_length > VOICE_LINK_MAX_PAYLOAD_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t required = VOICE_LINK_WIRE_OVERHEAD_BYTES + payload_length;
    if (output_capacity < required) {
        return ESP_ERR_INVALID_SIZE;
    }

    output[0] = VOICE_LINK_MAGIC_0;
    output[1] = VOICE_LINK_MAGIC_1;
    output[2] = VOICE_LINK_VERSION;
    output[3] = type;
    output[4] = flags;
    write_u16_le(&output[5], sequence);
    write_u16_le(&output[7], payload_length);
    write_u16_le(&output[9], voice_link_crc16(&output[2], 7U));
    if (payload_length > 0U) {
        memcpy(&output[11], payload, payload_length);
    }
    write_u16_le(&output[11U + payload_length],
                 voice_link_crc16(payload, payload_length));
    *output_length = required;
    return ESP_OK;
}

void voice_link_parser_begin(voice_link_parser_t *parser)
{
    if (parser == NULL) return;
    memset(parser, 0, sizeof(*parser));
}

voice_link_parse_result_t voice_link_parser_feed(
    voice_link_parser_t *parser,
    uint8_t byte,
    voice_link_packet_t *packet)
{
    if (parser == NULL || packet == NULL) return VOICE_LINK_PARSE_ERROR;

    if (parser->header_used < sizeof(parser->header)) {
        parser->header[parser->header_used++] = byte;
        if (parser->header_used < sizeof(parser->header)) {
            return VOICE_LINK_PARSE_MORE;
        }
        if (parser->header[0] != VOICE_LINK_VERSION ||
            read_u16_le(&parser->header[7]) !=
                voice_link_crc16(parser->header, 7U)) {
            return VOICE_LINK_PARSE_ERROR;
        }
        parser->packet.type = parser->header[1];
        parser->packet.flags = parser->header[2];
        parser->packet.sequence = read_u16_le(&parser->header[3]);
        parser->packet.payload_length = read_u16_le(&parser->header[5]);
        if (parser->packet.payload_length > VOICE_LINK_MAX_PAYLOAD_BYTES) {
            return VOICE_LINK_PARSE_ERROR;
        }
        return VOICE_LINK_PARSE_MORE;
    }

    if (parser->payload_used < parser->packet.payload_length) {
        parser->packet.payload[parser->payload_used++] = byte;
        return VOICE_LINK_PARSE_MORE;
    }

    parser->payload_crc[parser->payload_crc_used++] = byte;
    if (parser->payload_crc_used < sizeof(parser->payload_crc)) {
        return VOICE_LINK_PARSE_MORE;
    }
    if (read_u16_le(parser->payload_crc) !=
        voice_link_crc16(parser->packet.payload,
                         parser->packet.payload_length)) {
        return VOICE_LINK_PARSE_ERROR;
    }

    *packet = parser->packet;
    return VOICE_LINK_PARSE_COMPLETE;
}
