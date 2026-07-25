#include "ai_worker.hpp"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "vision_config.hpp"

namespace {
constexpr char TAG[] = "ai_worker";
constexpr int64_t WARNING_INTERVAL_MS = 5000;
constexpr size_t RGB_BUFFER_BYTES =
    static_cast<size_t>(vision_config::kFrameWidth) * vision_config::kFrameHeight * 3U;
} // namespace

bool AiWorker::init(HandAi &hand_ai)
{
    hand_ai_ = &hand_ai;
    jpeg_buffer_ = static_cast<uint8_t *>(
        heap_caps_malloc(vision_config::kAiJpegBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    rgb_buffer_ = static_cast<uint8_t *>(
        heap_caps_malloc(RGB_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    request_queue_ = xQueueCreate(1, sizeof(Request));
    result_queue_ = xQueueCreate(1, sizeof(AiInferenceResult));
    if (jpeg_buffer_ == nullptr || rgb_buffer_ == nullptr || request_queue_ == nullptr || result_queue_ == nullptr) {
        ESP_LOGE(TAG, "failed to allocate fixed AI buffers or queues");
        release_resources();
        return false;
    }

    const BaseType_t task_result = xTaskCreatePinnedToCore(task_entry,
                                                           "HandAiTask",
                                                           vision_config::kAiWorkerTaskStackBytes,
                                                           this,
                                                           vision_config::kAiWorkerTaskPriority,
                                                           &task_handle_,
                                                           vision_config::kAiWorkerCore);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "failed to create AI worker task");
        release_resources();
        return false;
    }
    initialized_ = true;
    ESP_LOGI(TAG,
             "AI worker ready: interval=%lld ms, core=%d",
             vision_config::kAiIntervalMs,
             vision_config::kAiWorkerCore);
    return true;
}

bool AiWorker::submit_jpeg(const uint8_t *jpeg,
                           size_t jpeg_size,
                           uint16_t width,
                           uint16_t height,
                           int64_t captured_ms)
{
    if (!initialized_ || jpeg == nullptr || jpeg_size < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        return false;
    }
    if (jpeg_size > vision_config::kAiJpegBufferBytes) {
        const int64_t current_ms = esp_timer_get_time() / 1000;
        if (current_ms - last_submit_warning_ms_ >= WARNING_INTERVAL_MS) {
            ESP_LOGW(TAG, "JPEG too large for AI input: %u bytes", static_cast<unsigned>(jpeg_size));
            last_submit_warning_ms_ = current_ms;
        }
        return false;
    }

    bool expected = false;
    if (!input_busy_.compare_exchange_strong(expected, true)) {
        return false;
    }

    std::memcpy(jpeg_buffer_, jpeg, jpeg_size);
    const Request request = {
        .jpeg_size = jpeg_size,
        .width = width,
        .height = height,
        .captured_ms = captured_ms,
    };
    if (xQueueSend(request_queue_, &request, 0) != pdTRUE) {
        input_busy_.store(false);
        return false;
    }
    return true;
}

bool AiWorker::receive(AiInferenceResult &result)
{
    return initialized_ && xQueueReceive(result_queue_, &result, 0) == pdTRUE;
}

void AiWorker::task_entry(void *context)
{
    static_cast<AiWorker *>(context)->task_loop();
}

void AiWorker::task_loop()
{
    Request request = {};
    while (true) {
        if (xQueueReceive(request_queue_, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        AiInferenceResult result = {};
        result.frame_width = request.width;
        result.frame_height = request.height;
        result.captured_ms = request.captured_ms;
        result.started_ms = esp_timer_get_time() / 1000;
        const size_t required_rgb_bytes = static_cast<size_t>(request.width) * request.height * 3U;
        if (required_rgb_bytes <= RGB_BUFFER_BYTES &&
            fmt2rgb888(jpeg_buffer_, request.jpeg_size, PIXFORMAT_JPEG, rgb_buffer_)) {
            result.observation = hand_ai_->infer(rgb_buffer_, request.width, request.height);
            result.valid = true;
        } else {
            ESP_LOGW(TAG, "JPEG decode failed; dropping AI sample");
        }
        result.finished_ms = esp_timer_get_time() / 1000;

        xQueueOverwrite(result_queue_, &result);
        input_busy_.store(false);
        taskYIELD();
    }
}

void AiWorker::release_resources()
{
    if (request_queue_ != nullptr) {
        vQueueDelete(request_queue_);
        request_queue_ = nullptr;
    }
    if (result_queue_ != nullptr) {
        vQueueDelete(result_queue_);
        result_queue_ = nullptr;
    }
    if (jpeg_buffer_ != nullptr) {
        heap_caps_free(jpeg_buffer_);
        jpeg_buffer_ = nullptr;
    }
    if (rgb_buffer_ != nullptr) {
        heap_caps_free(rgb_buffer_);
        rgb_buffer_ = nullptr;
    }
}
