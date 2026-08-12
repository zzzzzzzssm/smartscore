#include "voice_ima_adpcm.h"

#include <stdbool.h>

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

static uint8_t encode_nibble(int16_t sample, int *predictor, int *index)
{
    int difference = (int)sample - *predictor;
    uint8_t code = 0;
    if (difference < 0) {
        code = 8U;
        difference = -difference;
    }

    int step = s_step_table[*index];
    int delta = step >> 3;
    if (difference >= step) {
        code |= 4U;
        difference -= step;
        delta += step;
    }
    if (difference >= (step >> 1)) {
        code |= 2U;
        difference -= step >> 1;
        delta += step >> 1;
    }
    if (difference >= (step >> 2)) {
        code |= 1U;
        delta += step >> 2;
    }

    *predictor += (code & 8U) != 0U ? -delta : delta;
    if (*predictor > 32767) *predictor = 32767;
    if (*predictor < -32768) *predictor = -32768;
    *index += s_index_table[code];
    if (*index < 0) *index = 0;
    if (*index > 88) *index = 88;
    return code;
}

esp_err_t voice_ima_adpcm_encode(const int16_t *pcm,
                                 size_t sample_count,
                                 uint8_t *output,
                                 size_t output_capacity,
                                 size_t *output_length)
{
    if (pcm == NULL || output == NULL || output_length == NULL ||
        sample_count == 0U || sample_count > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t required = 6U + sample_count / 2U;
    if (output_capacity < required) return ESP_ERR_INVALID_SIZE;

    output[0] = (uint8_t)(sample_count & 0xFFU);
    output[1] = (uint8_t)(sample_count >> 8U);
    output[2] = (uint8_t)((uint16_t)pcm[0] & 0xFFU);
    output[3] = (uint8_t)((uint16_t)pcm[0] >> 8U);
    output[4] = 0;
    output[5] = 0;

    int predictor = pcm[0];
    int index = 0;
    size_t output_index = 6U;
    bool low_nibble = true;
    for (size_t sample_index = 1U; sample_index < sample_count;
         ++sample_index) {
        uint8_t nibble = encode_nibble(pcm[sample_index],
                                       &predictor, &index);
        if (low_nibble) {
            output[output_index] = nibble;
        } else {
            output[output_index++] |= (uint8_t)(nibble << 4U);
        }
        low_nibble = !low_nibble;
    }
    if (!low_nibble) ++output_index;
    *output_length = output_index;
    return ESP_OK;
}
