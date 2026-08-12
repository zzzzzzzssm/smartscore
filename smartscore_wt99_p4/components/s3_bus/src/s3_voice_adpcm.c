#include "s3_voice_adpcm.h"

static const int s_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
};

static const int8_t s_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

static int16_t decode_nibble(uint8_t code, int *predictor, int *index)
{
    const int step = s_step_table[*index];
    int delta = step >> 3;
    if ((code & 4U) != 0U) delta += step;
    if ((code & 2U) != 0U) delta += step >> 1;
    if ((code & 1U) != 0U) delta += step >> 2;
    *predictor += (code & 8U) != 0U ? -delta : delta;
    if (*predictor > 32767) *predictor = 32767;
    if (*predictor < -32768) *predictor = -32768;
    *index += s_index_table[code & 0x0FU];
    if (*index < 0) *index = 0;
    if (*index > 88) *index = 88;
    return (int16_t)*predictor;
}

esp_err_t s3_voice_adpcm_decode(const uint8_t *input,
                                size_t input_length,
                                int16_t *pcm,
                                size_t pcm_capacity,
                                size_t *sample_count)
{
    if (input == NULL || pcm == NULL || sample_count == NULL ||
        input_length < 6U) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t samples = (size_t)input[0] | ((size_t)input[1] << 8U);
    const size_t required = 6U + samples / 2U;
    if (samples == 0U || samples > pcm_capacity || input_length != required ||
        input[4] > 88U) {
        return ESP_ERR_INVALID_SIZE;
    }

    int predictor = (int16_t)((uint16_t)input[2] |
                              ((uint16_t)input[3] << 8U));
    int index = input[4];
    pcm[0] = (int16_t)predictor;
    for (size_t sample = 1U; sample < samples; ++sample) {
        uint8_t packed = input[6U + (sample - 1U) / 2U];
        uint8_t code = ((sample - 1U) & 1U) == 0U
                           ? packed & 0x0FU
                           : packed >> 4U;
        pcm[sample] = decode_nibble(code, &predictor, &index);
    }
    *sample_count = samples;
    return ESP_OK;
}
