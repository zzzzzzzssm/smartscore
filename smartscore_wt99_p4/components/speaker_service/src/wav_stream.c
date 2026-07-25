#include "wav_stream.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <string.h>

#include "board_sdcard.h"

static uint16_t read_u16(const uint8_t *value)
{
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static uint32_t read_u32(const uint8_t *value)
{
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
           ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

static bool has_wav_extension(const char *name)
{
    size_t length = name == NULL ? 0 : strlen(name);
    if (length < 5) {
        return false;
    }
    const char *extension = name + length - 4;
    return extension[0] == '.' &&
           tolower((unsigned char)extension[1]) == 'w' &&
           tolower((unsigned char)extension[2]) == 'a' &&
           tolower((unsigned char)extension[3]) == 'v';
}

static bool is_safe_file_name(const char *name)
{
    return name != NULL && name[0] != '\0' &&
           strlen(name) < WAV_STREAM_MAX_NAME &&
           strstr(name, "..") == NULL && strchr(name, '/') == NULL &&
           strchr(name, '\\') == NULL && has_wav_extension(name);
}

static unsigned char fold_ascii(unsigned char value)
{
    return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A'))
                                        : value;
}

static bool file_name_matches(const char *name, const char *search)
{
    if (search == NULL || search[0] == '\0') {
        return true;
    }
    size_t name_length = strlen(name);
    size_t search_length = strlen(search);
    if (search_length > name_length) {
        return false;
    }
    for (size_t start = 0; start + search_length <= name_length; ++start) {
        bool matched = true;
        for (size_t index = 0; index < search_length; ++index) {
            unsigned char left = (unsigned char)name[start + index];
            unsigned char right = (unsigned char)search[index];
            if (left < 0x80 && right < 0x80) {
                left = fold_ascii(left);
                right = fold_ascii(right);
            }
            if (left != right) {
                matched = false;
                break;
            }
        }
        if (matched) {
            return true;
        }
    }
    return false;
}

static bool is_listed_file(const struct dirent *entry, const char *search)
{
    return entry != NULL && entry->d_name[0] != '.' &&
           has_wav_extension(entry->d_name) &&
           file_name_matches(entry->d_name, search);
}

esp_err_t wav_stream_list(char names[][WAV_STREAM_MAX_NAME],
                          size_t capacity,
                          size_t *out_count)
{
    size_t total = 0;
    size_t page = 1;
    return wav_stream_list_page(NULL, 1, capacity, names, capacity,
                                out_count, &total, &page);
}

esp_err_t wav_stream_list_page(const char *search,
                               size_t requested_page,
                               size_t page_size,
                               char names[][WAV_STREAM_MAX_NAME],
                               size_t capacity,
                               size_t *out_count,
                               size_t *out_total,
                               size_t *out_page)
{
    board_sdcard_status_t card;
    board_sdcard_get_status(&card);
    if (!card.mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (names == NULL || out_count == NULL || out_total == NULL ||
        out_page == NULL || requested_page == 0 || page_size == 0 ||
        capacity < page_size) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_count = 0;
    *out_total = 0;
    *out_page = 1;
    DIR *directory = opendir(BOARD_SDCARD_WAV_DIRECTORY);
    if (directory == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (is_listed_file(entry, search)) {
            ++(*out_total);
        }
    }
    closedir(directory);

    size_t total_pages = (*out_total + page_size - 1) / page_size;
    *out_page = total_pages == 0
                    ? 1
                    : (requested_page > total_pages ? total_pages
                                                     : requested_page);
    size_t first_index = (*out_page - 1) * page_size;
    directory = opendir(BOARD_SDCARD_WAV_DIRECTORY);
    if (directory == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t matched_index = 0;
    while (*out_count < page_size && (entry = readdir(directory)) != NULL) {
        if (!is_listed_file(entry, search)) {
            continue;
        }
        if (matched_index++ < first_index) {
            continue;
        }
        strlcpy(names[*out_count], entry->d_name, WAV_STREAM_MAX_NAME);
        ++(*out_count);
    }
    closedir(directory);
    return ESP_OK;
}

esp_err_t wav_stream_open(const char *safe_name, wav_stream_file_t *out_file)
{
    board_sdcard_status_t card;
    board_sdcard_get_status(&card);
    if (!card.mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!is_safe_file_name(safe_name) || out_file == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_file, 0, sizeof(*out_file));
    char path[sizeof(BOARD_SDCARD_WAV_DIRECTORY) + WAV_STREAM_MAX_NAME + 2];
    snprintf(path, sizeof(path), "%s/%s", BOARD_SDCARD_WAV_DIRECTORY, safe_name);
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t riff[12];
    if (fread(riff, 1, sizeof(riff), file) != sizeof(riff) ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool format_found = false;
    bool data_found = false;
    uint16_t format = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t sample_rate = 0;
    uint32_t data_bytes = 0;
    for (int chunk_count = 0; chunk_count < 32 && !data_found; ++chunk_count) {
        uint8_t header[8];
        if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
            break;
        }
        uint32_t size = read_u32(header + 4);
        if (memcmp(header, "fmt ", 4) == 0) {
            if (size < 16 || size > 128) {
                break;
            }
            uint8_t details[128];
            if (fread(details, 1, size, file) != size) {
                break;
            }
            format = read_u16(details);
            channels = read_u16(details + 2);
            sample_rate = read_u32(details + 4);
            bits = read_u16(details + 14);
            format_found = true;
            if ((size & 1U) != 0 && fseek(file, 1, SEEK_CUR) != 0) {
                break;
            }
        } else if (memcmp(header, "data", 4) == 0) {
            if (!format_found) {
                break;
            }
            data_bytes = size;
            data_found = true;
        } else if (fseek(file, (long)(size + (size & 1U)), SEEK_CUR) != 0) {
            break;
        }
    }

    if (!data_found || format != 1 || channels != 1 || bits != 16 ||
        (sample_rate != 16000 && sample_rate != 24000) ||
        (data_bytes & 1U) != 0) {
        fclose(file);
        return ESP_ERR_NOT_SUPPORTED;
    }

    out_file->file = file;
    out_file->sample_rate_hz = sample_rate;
    out_file->data_bytes = data_bytes;
    out_file->remaining_bytes = data_bytes;
    strlcpy(out_file->name, safe_name, sizeof(out_file->name));
    return ESP_OK;
}

esp_err_t wav_stream_read(wav_stream_file_t *stream,
                          void *buffer,
                          size_t capacity,
                          size_t *out_bytes)
{
    if (stream == NULL || stream->file == NULL || buffer == NULL ||
        out_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t requested = capacity < stream->remaining_bytes
                           ? capacity
                           : stream->remaining_bytes;
    requested &= ~(size_t)1;
    if (requested == 0) {
        *out_bytes = 0;
        return ESP_OK;
    }
    size_t read = fread(buffer, 1, requested, stream->file);
    if (read != requested) {
        return ESP_ERR_INVALID_SIZE;
    }
    stream->remaining_bytes -= (uint32_t)read;
    *out_bytes = read;
    return ESP_OK;
}

void wav_stream_close(wav_stream_file_t *stream)
{
    if (stream != NULL && stream->file != NULL) {
        fclose(stream->file);
    }
    if (stream != NULL) {
        memset(stream, 0, sizeof(*stream));
    }
}
