#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "sdmmc_cmd.h"
#include "vision_config.hpp"

class SdStorage {
public:
    esp_err_t init();
    bool start_session(int64_t start_time_ms, const char *practice_session_id);
    bool finish_session(int64_t end_time_ms);

    bool can_accept_photo();
    void enqueue_jpeg(uint8_t *jpeg_data, size_t jpeg_size);
    bool writer_idle() const;
    bool saving_available() const;
    bool session_active() const;
    uint32_t saved_photo_count() const;
    bool find_photo_session(const char *practice_session_id,
                            uint32_t *out_photo_count,
                            char *out_directory,
                            size_t directory_capacity) const;
    bool photo_file_info(const char *practice_session_id,
                         uint32_t photo_index,
                         char *out_path,
                         size_t path_capacity,
                         size_t *out_size) const;

private:
    struct WriteItem {
        uint8_t *data;
        size_t size;
        char path[vision_config::kPhotoPathBufferSize];
    };

    static void writer_task_entry(void *context);
    void writer_task();
    bool write_jpeg_file(const WriteItem &item);
    void handle_write_failure();
    void drain_pending_items();
    bool write_session_file(int64_t end_time_ms);
    uint32_t find_next_session_id() const;
    void warn_queue_limited(const char *message);

    sdmmc_card_t *card_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t writer_task_handle_ = nullptr;
    std::atomic<bool> card_available_{false};
    std::atomic<bool> saving_available_{false};
    std::atomic<uint32_t> pending_count_{0};
    std::atomic<uint32_t> saved_count_{0};
    bool session_active_ = false;
    uint32_t session_id_ = 0;
    uint32_t next_photo_index_ = 0;
    int64_t session_start_time_ms_ = 0;
    int64_t last_queue_warning_ms_ = -vision_config::kQueueWarningIntervalMs;
    char session_directory_[vision_config::kSessionPathBufferSize] = {};
    char practice_session_id_[64] = {};
};
