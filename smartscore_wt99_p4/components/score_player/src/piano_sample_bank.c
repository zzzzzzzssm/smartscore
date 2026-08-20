#include "piano_sample_bank.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#define PIANO_BANK_VERSION 1U
#define PIANO_BANK_HEADER_SIZE 32U
#define PIANO_BANK_ENTRY_SIZE 32U
#define PIANO_BANK_MAX_ZONES 128U
#define PIANO_BANK_MAX_SAMPLE_BYTES (6U * 1024U * 1024U)

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint16_t version;
    uint16_t header_size;
    uint32_t sample_rate;
    uint16_t zone_count;
    uint16_t entry_size;
    uint32_t index_crc32;
    uint32_t payload_offset;
    uint32_t file_size;
    uint32_t flags;
} piano_bank_header_disk_t;

typedef struct __attribute__((packed)) {
    uint8_t root_midi;
    uint8_t velocity_min;
    uint8_t velocity_max;
    uint8_t reserved0;
    uint32_t data_offset;
    uint32_t sample_count;
    uint32_t loop_start;
    uint32_t loop_end;
    int16_t gain_q15;
    uint16_t reserved1;
    uint32_t sample_crc32;
    uint32_t flags;
} piano_bank_zone_disk_t;

_Static_assert(sizeof(piano_bank_header_disk_t) == PIANO_BANK_HEADER_SIZE,
               "piano bank header size changed");
_Static_assert(sizeof(piano_bank_zone_disk_t) == PIANO_BANK_ENTRY_SIZE,
               "piano bank entry size changed");

struct piano_sample_bank {
    piano_sample_zone_t *zones;
    size_t zone_count;
    int16_t *sample_memory;
    size_t sample_bytes;
};

static void set_error(char *error, size_t error_size, const char *message)
{
    if (!error || error_size == 0U) return;
    snprintf(error, error_size, "%s", message ? message : "");
}

static uint32_t crc32_bytes(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_C(0xFFFFFFFF);
    for (size_t index = 0; index < size; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0; bit < 8U; ++bit) {
            const uint32_t mask = UINT32_C(0) - (crc & UINT32_C(1));
            crc = (crc >> 1U) ^ (UINT32_C(0xEDB88320) & mask);
        }
    }
    return ~crc;
}

static bool read_exact(FILE *file, void *data, size_t size)
{
    return file && data && fread(data, 1U, size, file) == size;
}

static bool zone_is_valid(const piano_bank_zone_disk_t *zone,
                          const piano_bank_header_disk_t *header)
{
    if (!zone || !header || zone->root_midi > 127U ||
        zone->velocity_min == 0U ||
        zone->velocity_max < zone->velocity_min ||
        zone->sample_count < 2U || zone->loop_start >= zone->loop_end ||
        zone->loop_end >= zone->sample_count ||
        zone->loop_end >= UINT16_MAX || zone->gain_q15 <= 0) {
        return false;
    }
    const uint64_t data_end = (uint64_t)zone->data_offset +
                              (uint64_t)zone->sample_count * sizeof(int16_t);
    return zone->data_offset >= header->payload_offset &&
           data_end <= header->file_size;
}

static int velocity_distance(const piano_bank_zone_disk_t *zone,
                             uint8_t velocity)
{
    if (velocity < zone->velocity_min)
        return (int)zone->velocity_min - velocity;
    if (velocity > zone->velocity_max)
        return (int)velocity - zone->velocity_max;
    return 0;
}

static int select_disk_zone(const piano_bank_zone_disk_t *zones,
                            size_t zone_count,
                            uint8_t midi, uint8_t velocity)
{
    int best_index = -1;
    int best_velocity_distance = INT_MAX;
    int best_note_distance = INT_MAX;
    for (size_t index = 0; index < zone_count; ++index) {
        const int layer_distance = velocity_distance(&zones[index], velocity);
        const int note_distance = abs((int)midi - zones[index].root_midi);
        if (layer_distance < best_velocity_distance ||
            (layer_distance == best_velocity_distance &&
             note_distance < best_note_distance)) {
            best_velocity_distance = layer_distance;
            best_note_distance = note_distance;
            best_index = (int)index;
        }
    }
    return best_note_distance <= 1 ? best_index : -1;
}

static int select_loaded_zone(const piano_sample_bank_t *bank,
                              uint8_t midi, uint8_t velocity)
{
    int best_index = -1;
    int best_velocity_distance = INT_MAX;
    int best_note_distance = INT_MAX;
    for (size_t index = 0; index < bank->zone_count; ++index) {
        const piano_sample_zone_t *zone = &bank->zones[index];
        int layer_distance = 0;
        if (velocity < zone->velocity_min)
            layer_distance = (int)zone->velocity_min - velocity;
        else if (velocity > zone->velocity_max)
            layer_distance = (int)velocity - zone->velocity_max;
        const int note_distance = abs((int)midi - zone->root_midi);
        if (layer_distance < best_velocity_distance ||
            (layer_distance == best_velocity_distance &&
             note_distance < best_note_distance)) {
            best_velocity_distance = layer_distance;
            best_note_distance = note_distance;
            best_index = (int)index;
        }
    }
    return best_note_distance <= 1 ? best_index : -1;
}

void piano_sample_bank_free(piano_sample_bank_t *bank)
{
    if (!bank) return;
    free(bank->sample_memory);
    free(bank->zones);
    free(bank);
}

esp_err_t piano_sample_bank_load(
    const char *path,
    const piano_sample_request_t *requests,
    size_t request_count,
    piano_sample_bank_cancel_cb_t cancel_callback,
    void *cancel_user_data,
    piano_sample_bank_t **out_bank,
    char *error,
    size_t error_size)
{
    if (out_bank) *out_bank = NULL;
    set_error(error, error_size, "");
    if (!path || !requests || request_count == 0U || !out_bank) {
        set_error(error, error_size, "invalid sample bank request");
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        set_error(error, error_size, "sample bank not found");
        return ESP_ERR_NOT_FOUND;
    }

    piano_bank_header_disk_t header;
    piano_bank_zone_disk_t *disk_zones = NULL;
    bool *selected = NULL;
    piano_sample_bank_t *bank = NULL;
    esp_err_t result = ESP_FAIL;

    if (fseek(file, 0, SEEK_END) != 0) {
        set_error(error, error_size, "unable to inspect sample bank");
        goto cleanup;
    }
    const long actual_file_size = ftell(file);
    if (actual_file_size < 0 || fseek(file, 0, SEEK_SET) != 0 ||
        !read_exact(file, &header, sizeof(header))) {
        set_error(error, error_size, "unable to read sample bank header");
        goto cleanup;
    }
    if ((uint64_t)actual_file_size > UINT32_MAX ||
        memcmp(header.magic, "SSPB", 4U) != 0 ||
        header.version != PIANO_BANK_VERSION ||
        header.header_size != PIANO_BANK_HEADER_SIZE ||
        header.entry_size != PIANO_BANK_ENTRY_SIZE ||
        header.sample_rate != PIANO_SAMPLE_BANK_RATE_HZ ||
        header.zone_count == 0U ||
        header.zone_count > PIANO_BANK_MAX_ZONES ||
        header.payload_offset != header.header_size +
                                 header.zone_count * header.entry_size ||
        header.file_size != (uint32_t)actual_file_size) {
        set_error(error, error_size, "unsupported sample bank format");
        result = ESP_ERR_INVALID_VERSION;
        goto cleanup;
    }

    const size_t index_bytes =
        (size_t)header.zone_count * sizeof(piano_bank_zone_disk_t);
    disk_zones = malloc(index_bytes);
    selected = calloc(header.zone_count, sizeof(*selected));
    if (!disk_zones || !selected) {
        set_error(error, error_size, "not enough memory for sample index");
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    if (!read_exact(file, disk_zones, index_bytes) ||
        crc32_bytes(disk_zones, index_bytes) != header.index_crc32) {
        set_error(error, error_size, "sample bank index CRC mismatch");
        result = ESP_ERR_INVALID_CRC;
        goto cleanup;
    }
    for (size_t index = 0; index < header.zone_count; ++index) {
        if (!zone_is_valid(&disk_zones[index], &header)) {
            set_error(error, error_size, "sample bank contains invalid zone");
            result = ESP_ERR_INVALID_RESPONSE;
            goto cleanup;
        }
    }

    size_t selected_count = 0U;
    size_t selected_bytes = 0U;
    for (size_t request = 0; request < request_count; ++request) {
        const uint8_t velocity = requests[request].velocity
                                     ? requests[request].velocity
                                     : 96U;
        const int zone_index = select_disk_zone(
            disk_zones, header.zone_count, requests[request].midi, velocity);
        if (zone_index < 0) {
            set_error(error, error_size,
                      "sample bank does not cover score note range");
            result = ESP_ERR_NOT_SUPPORTED;
            goto cleanup;
        }
        if (selected[zone_index]) continue;
        selected[zone_index] = true;
        selected_count++;
        const size_t bytes =
            (size_t)disk_zones[zone_index].sample_count * sizeof(int16_t);
        if (bytes > PIANO_BANK_MAX_SAMPLE_BYTES - selected_bytes) {
            set_error(error, error_size, "required samples exceed memory limit");
            result = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
        selected_bytes += bytes;
    }
    if (selected_count == 0U || selected_bytes == 0U) {
        set_error(error, error_size, "sample bank has no matching zones");
        result = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    bank = calloc(1U, sizeof(*bank));
    if (!bank) {
        set_error(error, error_size, "not enough memory for sample bank");
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    bank->zones = calloc(selected_count, sizeof(*bank->zones));
    bank->sample_memory = heap_caps_malloc(
        selected_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bank->zones || !bank->sample_memory) {
        set_error(error, error_size, "not enough PSRAM for piano samples");
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    bank->sample_bytes = selected_bytes;

    uint8_t *write_pointer = (uint8_t *)bank->sample_memory;
    for (size_t index = 0; index < header.zone_count; ++index) {
        if (!selected[index]) continue;
        if (cancel_callback && cancel_callback(cancel_user_data)) {
            set_error(error, error_size, "sample bank load cancelled");
            result = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }
        const piano_bank_zone_disk_t *source = &disk_zones[index];
        const size_t bytes = (size_t)source->sample_count * sizeof(int16_t);
        if (fseek(file, (long)source->data_offset, SEEK_SET) != 0 ||
            !read_exact(file, write_pointer, bytes)) {
            set_error(error, error_size, "unable to read piano sample data");
            result = ESP_FAIL;
            goto cleanup;
        }
        if (crc32_bytes(write_pointer, bytes) != source->sample_crc32) {
            set_error(error, error_size, "piano sample CRC mismatch");
            result = ESP_ERR_INVALID_CRC;
            goto cleanup;
        }
        piano_sample_zone_t *destination = &bank->zones[bank->zone_count++];
        *destination = (piano_sample_zone_t) {
            .samples = (const int16_t *)write_pointer,
            .sample_count = source->sample_count,
            .loop_start = source->loop_start,
            .loop_end = source->loop_end,
            .root_midi = source->root_midi,
            .velocity_min = source->velocity_min,
            .velocity_max = source->velocity_max,
            .gain = (float)source->gain_q15 / 32768.0f,
        };
        write_pointer += bytes;
    }

    *out_bank = bank;
    bank = NULL;
    result = ESP_OK;

cleanup:
    piano_sample_bank_free(bank);
    free(selected);
    free(disk_zones);
    fclose(file);
    return result;
}

const piano_sample_zone_t *piano_sample_bank_select(
    const piano_sample_bank_t *bank, uint8_t midi, uint8_t velocity)
{
    if (!bank || !bank->zones || bank->zone_count == 0U) return NULL;
    if (velocity == 0U) velocity = 96U;
    const int index = select_loaded_zone(bank, midi, velocity);
    return index >= 0 ? &bank->zones[index] : NULL;
}

size_t piano_sample_bank_memory_bytes(const piano_sample_bank_t *bank)
{
    return bank ? bank->sample_bytes : 0U;
}

size_t piano_sample_bank_zone_count(const piano_sample_bank_t *bank)
{
    return bank ? bank->zone_count : 0U;
}
