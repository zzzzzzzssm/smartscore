#include "sd_storage.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include "board_pins.hpp"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/task.h"
#include "vision_config.hpp"

namespace {
constexpr char TAG[] = "sd_storage";

bool ensure_directory(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return true;
    }
    ESP_LOGE(TAG, "mkdir(%s) failed: errno=%d", path, errno);
    return false;
}

bool valid_practice_session_id(const char *value)
{
    if (value == nullptr || value[0] == '\0' || std::strlen(value) >= 64) {
        return false;
    }
    for (const unsigned char *cursor =
             reinterpret_cast<const unsigned char *>(value);
         *cursor != '\0'; ++cursor) {
        const bool allowed = (*cursor >= 'a' && *cursor <= 'z') ||
                             (*cursor >= 'A' && *cursor <= 'Z') ||
                             (*cursor >= '0' && *cursor <= '9') ||
                             *cursor == '-' || *cursor == '_';
        if (!allowed) return false;
    }
    return true;
}
} // namespace

esp_err_t SdStorage::init()
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = board::SDMMC_PIN_CLK;
    slot.cmd = board::SDMMC_PIN_CMD;
    slot.d0 = board::SDMMC_PIN_D0;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.d4 = GPIO_NUM_NC;
    slot.d5 = GPIO_NUM_NC;
    slot.d6 = GPIO_NUM_NC;
    slot.d7 = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = vision_config::kSdMaxOpenFiles;
    mount_config.allocation_unit_size = vision_config::kSdAllocationUnitBytes;
    mount_config.disk_status_check_enable = true;
    mount_config.use_one_fat = false;
    //mount_config.rootdir_entries = 0;

    const esp_err_t err = esp_vfs_fat_sdmmc_mount(
        vision_config::kSdMountPoint, &host, &slot, &mount_config, &card_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC mount failed: %s; photo saving disabled", esp_err_to_name(err));
        return err;
    }

    if (!ensure_directory(vision_config::kStorageRoot)) {
        ESP_LOGE(TAG, "storage root unavailable; photo saving disabled");
        return ESP_FAIL;
    }

    queue_ = xQueueCreate(vision_config::kSdWriterQueueLength, sizeof(WriteItem));
    if (queue_ == nullptr) {
        ESP_LOGE(TAG, "failed to create SD writer queue");
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t task_result = xTaskCreate(writer_task_entry,
                                                "SDWriterTask",
                                                vision_config::kSdWriterTaskStackBytes,
                                                this,
                                                vision_config::kSdWriterTaskPriority,
                                                &writer_task_handle_);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "failed to create SD writer task");
        vQueueDelete(queue_);
        queue_ = nullptr;
        return ESP_ERR_NO_MEM;
    }

    card_available_.store(true);
    saving_available_.store(true);
    ESP_LOGI(TAG, "SD card mounted at %s using SDMMC 1-bit", vision_config::kSdMountPoint);
    sdmmc_card_print_info(stdout, card_);
    return ESP_OK;
}

bool SdStorage::start_session(int64_t start_time_ms,
                              const char *practice_session_id)
{
    if (!saving_available_.load() || queue_ == nullptr || !writer_idle()) {
        return false;
    }

    session_id_ = find_next_session_id();
    if (session_id_ == 0 || session_id_ > 9999) {
        ESP_LOGE(TAG, "no FAT 8.3 session directory name available (S0001-S9999)");
        return false;
    }
    const int path_length = std::snprintf(session_directory_,
                                          sizeof(session_directory_),
                                          "%s/S%04" PRIu32,
                                          vision_config::kStorageRoot,
                                          session_id_);
    if (path_length < 0 || static_cast<size_t>(path_length) >= sizeof(session_directory_) ||
        !ensure_directory(session_directory_)) {
        handle_write_failure();
        return false;
    }

    saved_count_.store(0);
    next_photo_index_ = 0;
    session_start_time_ms_ = start_time_ms;
    practice_session_id_[0] = '\0';
    if (valid_practice_session_id(practice_session_id)) {
        std::snprintf(practice_session_id_, sizeof(practice_session_id_),
                      "%s", practice_session_id);
    }
    session_active_ = true;
    if (!write_session_file(0)) {
        session_active_ = false;
        handle_write_failure();
        return false;
    }

    ESP_LOGI(TAG, "session %" PRIu32 " started: %s", session_id_, session_directory_);
    return true;
}

bool SdStorage::finish_session(int64_t end_time_ms)
{
    if (!session_active_ || !writer_idle()) {
        return false;
    }

    bool metadata_ok = true;
    if (card_available_.load()) {
        metadata_ok = write_session_file(end_time_ms);
        if (!metadata_ok) {
            handle_write_failure();
        }
    }
    ESP_LOGI(TAG,
             "session %" PRIu32 " finished, saved=%" PRIu32,
             session_id_,
             saved_count_.load());
    session_active_ = false;
    return metadata_ok;
}

bool SdStorage::can_accept_photo()
{
    if (!session_active_ || !saving_available_.load() || queue_ == nullptr) {
        return false;
    }
    if (uxQueueSpacesAvailable(queue_) == 0) {
        warn_queue_limited("SD writer queue full; dropping current JPEG");
        return false;
    }
    return true;
}

void SdStorage::enqueue_jpeg(uint8_t *jpeg_data, size_t jpeg_size)
{
    if (jpeg_data == nullptr) {
        return;
    }
    if (!can_accept_photo()) {
        heap_caps_free(jpeg_data);
        warn_queue_limited("SD writer queue full; dropping current JPEG");
        return;
    }
    if (next_photo_index_ >= 999999) {
        ESP_LOGE(TAG, "FAT 8.3 photo index limit reached; dropping JPEG");
        heap_caps_free(jpeg_data);
        return;
    }

    WriteItem item = {};
    item.data = jpeg_data;
    item.size = jpeg_size;
    const uint32_t photo_index = next_photo_index_ + 1;
    const int path_length = std::snprintf(item.path,
                                          sizeof(item.path),
                                          "%s/I%06" PRIu32 ".JPG",
                                          session_directory_,
                                          photo_index);
    if (path_length < 0 || static_cast<size_t>(path_length) >= sizeof(item.path)) {
        ESP_LOGE(TAG, "photo path exceeds buffer; dropping JPEG");
        heap_caps_free(jpeg_data);
        return;
    }

    pending_count_.fetch_add(1);
    if (xQueueSend(queue_, &item, 0) != pdTRUE) {
        pending_count_.fetch_sub(1);
        heap_caps_free(jpeg_data);
        warn_queue_limited("SD writer queue send failed; dropping current JPEG");
        return;
    }
    next_photo_index_ = photo_index;
}

bool SdStorage::writer_idle() const
{
    return pending_count_.load() == 0;
}

bool SdStorage::saving_available() const
{
    return saving_available_.load();
}

bool SdStorage::session_active() const
{
    return session_active_;
}

uint32_t SdStorage::saved_photo_count() const
{
    return saved_count_.load();
}

bool SdStorage::find_photo_session(const char *practice_session_id,
                                   uint32_t *out_photo_count,
                                   char *out_directory,
                                   size_t directory_capacity) const
{
    if (!card_available_.load() ||
        !valid_practice_session_id(practice_session_id) ||
        out_photo_count == nullptr || out_directory == nullptr ||
        directory_capacity == 0) {
        return false;
    }

    DIR *root = opendir(vision_config::kStorageRoot);
    if (root == nullptr) return false;
    bool found = false;
    while (dirent *entry = readdir(root)) {
        uint32_t numeric_id = 0;
        char trailing = '\0';
        if (std::strlen(entry->d_name) != 5 ||
            std::sscanf(entry->d_name, "S%" SCNu32 "%c",
                        &numeric_id, &trailing) != 1 ||
            numeric_id == 0 || numeric_id > 9999) {
            continue;
        }
        char directory[vision_config::kSessionPathBufferSize] = {};
        char metadata[vision_config::kPhotoPathBufferSize] = {};
        if (std::snprintf(directory, sizeof(directory), "%s/%s",
                          vision_config::kStorageRoot, entry->d_name) <= 0 ||
            std::snprintf(metadata, sizeof(metadata), "%s/SESSION.TXT",
                          directory) <= 0) {
            continue;
        }
        FILE *file = std::fopen(metadata, "rb");
        if (file == nullptr) continue;
        char line[128] = {};
        char stored_session[64] = {};
        uint32_t photo_count = 0;
        while (std::fgets(line, sizeof(line), file) != nullptr) {
            if (std::strncmp(line, "practice_session_id=", 20) == 0) {
                const char *value = line + 20;
                const size_t length = std::strcspn(value, "\r\n");
                if (length == 0 || length >= sizeof(stored_session)) {
                    stored_session[0] = '\0';
                    continue;
                }
                std::memcpy(stored_session, value, length);
                stored_session[length] = '\0';
            } else {
                (void)std::sscanf(line, "photo_count=%" SCNu32,
                                  &photo_count);
            }
        }
        std::fclose(file);
        if (std::strcmp(stored_session, practice_session_id) == 0) {
            if (std::strlen(directory) + 1 > directory_capacity) break;
            std::snprintf(out_directory, directory_capacity, "%s", directory);
            *out_photo_count = photo_count;
            found = true;
            break;
        }
    }
    closedir(root);
    return found;
}

bool SdStorage::photo_file_info(const char *practice_session_id,
                                uint32_t photo_index,
                                char *out_path,
                                size_t path_capacity,
                                size_t *out_size) const
{
    if (photo_index == 0 || out_path == nullptr || path_capacity == 0 ||
        out_size == nullptr) {
        return false;
    }
    uint32_t photo_count = 0;
    char directory[vision_config::kSessionPathBufferSize] = {};
    if (!find_photo_session(practice_session_id, &photo_count,
                            directory, sizeof(directory)) ||
        photo_index > photo_count) {
        return false;
    }
    const int length = std::snprintf(out_path, path_capacity,
                                     "%s/I%06" PRIu32 ".JPG",
                                     directory, photo_index);
    if (length <= 0 || static_cast<size_t>(length) >= path_capacity) {
        return false;
    }
    struct stat info = {};
    if (stat(out_path, &info) != 0 || info.st_size <= 0) return false;
    *out_size = static_cast<size_t>(info.st_size);
    return true;
}

void SdStorage::writer_task_entry(void *context)
{
    static_cast<SdStorage *>(context)->writer_task();
}

void SdStorage::writer_task()
{
    WriteItem item = {};
    while (true) {
        if (xQueueReceive(queue_, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const bool success = saving_available_.load() && write_jpeg_file(item);
        heap_caps_free(item.data);
        item.data = nullptr;

        if (success) {
            saved_count_.fetch_add(1);
        } else {
            handle_write_failure();
        }
        pending_count_.fetch_sub(1);
    }
}

bool SdStorage::write_jpeg_file(const WriteItem &item)
{
    FILE *file = std::fopen(item.path, "wb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "fopen(%s) failed: errno=%d", item.path, errno);
        return false;
    }

    bool success = std::fwrite(item.data, 1, item.size, file) == item.size;
    if (!success) {
        ESP_LOGE(TAG, "short write for %s: errno=%d", item.path, errno);
    }
    if (std::fflush(file) != 0) {
        ESP_LOGE(TAG, "fflush(%s) failed: errno=%d", item.path, errno);
        success = false;
    }
    if (std::fclose(file) != 0) {
        ESP_LOGE(TAG, "fclose(%s) failed: errno=%d", item.path, errno);
        success = false;
    }
    return success;
}

void SdStorage::handle_write_failure()
{
    const bool was_available = saving_available_.exchange(false);
    card_available_.store(false);
    if (was_available) {
        ESP_LOGE(TAG, "SD write failed; disabling photo saving while gesture recognition continues");
    }
    drain_pending_items();
}

void SdStorage::drain_pending_items()
{
    WriteItem pending = {};
    while (xQueueReceive(queue_, &pending, 0) == pdTRUE) {
        heap_caps_free(pending.data);
        pending_count_.fetch_sub(1);
    }
}

bool SdStorage::write_session_file(int64_t end_time_ms)
{
    char path[vision_config::kPhotoPathBufferSize] = {};
    const int path_length = std::snprintf(path, sizeof(path), "%s/SESSION.TXT", session_directory_);
    if (path_length < 0 || static_cast<size_t>(path_length) >= sizeof(path)) {
        ESP_LOGE(TAG, "session metadata path exceeds buffer");
        return false;
    }

    FILE *file = std::fopen(path, "wb");
    if (file == nullptr) {
        ESP_LOGE(TAG, "fopen(%s) failed: errno=%d", path, errno);
        return false;
    }

    const int written = std::fprintf(file,
                                     "session_id=%" PRIu32 "\n"
                                     "practice_session_id=%s\n"
                                     "photo_interval_ms=%" PRId64 "\n"
                                     "photo_count=%" PRIu32 "\n"
                                     "start_time_ms=%" PRId64 "\n"
                                     "end_time_ms=%" PRId64 "\n",
                                     session_id_,
                                     practice_session_id_,
                                     vision_config::kPhotoIntervalMs,
                                     saved_count_.load(),
                                     session_start_time_ms_,
                                     end_time_ms);
    bool success = written > 0;
    if (std::fflush(file) != 0) {
        success = false;
    }
    if (std::fclose(file) != 0) {
        success = false;
    }
    if (!success) {
        ESP_LOGE(TAG, "failed to write session metadata: %s", path);
    }
    return success;
}

uint32_t SdStorage::find_next_session_id() const
{
    uint32_t maximum = 0;
    DIR *directory = opendir(vision_config::kStorageRoot);
    if (directory == nullptr) {
        return 1;
    }

    while (dirent *entry = readdir(directory)) {
        uint32_t id = 0;
        char trailing = '\0';
        if (std::strlen(entry->d_name) == 5 &&
            std::sscanf(entry->d_name, "S%" SCNu32 "%c", &id, &trailing) == 1 && id > 0 && id <= 9999 &&
            id > maximum) {
            maximum = id;
        }
    }
    closedir(directory);
    return maximum >= 9999 ? 0 : maximum + 1;
}

void SdStorage::warn_queue_limited(const char *message)
{
    const int64_t current_ms = esp_timer_get_time() / 1000;
    if (current_ms - last_queue_warning_ms_ >= vision_config::kQueueWarningIntervalMs) {
        ESP_LOGW(TAG, "%s", message);
        last_queue_warning_ms_ = current_ms;
    }
}
