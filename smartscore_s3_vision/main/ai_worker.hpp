#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hand_ai.hpp"

struct AiInferenceResult {
    HandObservation observation = {};
    uint16_t frame_width = 0;
    uint16_t frame_height = 0;
    int64_t captured_ms = 0;
    int64_t started_ms = 0;
    int64_t finished_ms = 0;
    bool valid = false;
};

class AiWorker {
public:
    bool init(HandAi &hand_ai);
    bool submit_jpeg(const uint8_t *jpeg,
                     size_t jpeg_size,
                     uint16_t width,
                     uint16_t height,
                     int64_t captured_ms);
    bool receive(AiInferenceResult &result);

private:
    struct Request {
        size_t jpeg_size;
        uint16_t width;
        uint16_t height;
        int64_t captured_ms;
    };

    static void task_entry(void *context);
    void task_loop();
    void release_resources();

    HandAi *hand_ai_ = nullptr;
    uint8_t *jpeg_buffer_ = nullptr;
    uint8_t *rgb_buffer_ = nullptr;
    QueueHandle_t request_queue_ = nullptr;
    QueueHandle_t result_queue_ = nullptr;
    TaskHandle_t task_handle_ = nullptr;
    std::atomic_bool input_busy_{false};
    bool initialized_ = false;
    int64_t last_submit_warning_ms_ = -5000;
};
