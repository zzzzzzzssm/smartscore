#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "ai_worker.hpp"
#include "camera_driver.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gesture_engine.hpp"
#include "hand_ai.hpp"
#include "p4_control_link.hpp"
#include "sd_storage.hpp"
#include "vision_config.hpp"

namespace {
constexpr char TAG[] = "smartscore";

int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

bool command_for_action(int action_id, P4ControlCommand &command)
{
    switch (action_id) {
    case 1:
        command = P4ControlCommand::PREVIOUS_PAGE;
        return true;
    case 2:
        command = P4ControlCommand::NEXT_PAGE;
        return true;
    case 3:
        command = P4ControlCommand::START_PRACTICE;
        return true;
    case 4:
        command = P4ControlCommand::PAUSE_PRACTICE;
        return true;
    case 5:
        command = P4ControlCommand::SHOW_SCORE;
        return true;
    default:
        return false;
    }
}

bool emit_action(int action_id)
{
    std::printf("%d\n", action_id);
    std::fflush(stdout);

    P4ControlCommand command = P4ControlCommand::START_PRACTICE;
    if (!command_for_action(action_id, command)) return false;
    const esp_err_t err = p4_control_link_send(command);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "unable to send action %d to P4: %s",
                 action_id, esp_err_to_name(err));
        return false;
    }
    return true;
}

void advance_periodic_deadline(int64_t &deadline_ms, int64_t period_ms, int64_t current_ms)
{
    do {
        deadline_ms += period_ms;
    } while (deadline_ms <= current_ms);
}

void log_memory()
{
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG,
             "memory free: internal=%u bytes, psram=%u bytes",
             static_cast<unsigned>(internal_free),
             static_cast<unsigned>(psram_free));
}

void log_ai_decision(const AiInferenceResult &result,
                     const GestureDecision &decision,
                     SessionState decision_state,
                     int64_t result_interval_ms)
{
    const HandObservation &observation = result.observation;
    ESP_LOGI(TAG,
             "AI label=%s conf=%.0f%% hand=%.0f%% zone=%s state=%s votes=%u/%u hold=%" PRId64
             "/%" PRId64 " reason=%s action=%d infer=%" PRId64 "ms total=%" PRId64
             "ms interval=%" PRId64 "ms hands=%u point_x=%.0f%% point_zone=%s margin=%.0f%%",
             observation.gesture_label == nullptr ? "not_classified" : observation.gesture_label,
             static_cast<double>(observation.gesture_confidence * 100.0F),
             static_cast<double>(observation.detection_confidence * 100.0F),
             observation.detected ? (observation.in_control_zone ? "yes" : "no") : "n/a",
             session_state_name(decision_state),
             static_cast<unsigned>(decision.votes),
             static_cast<unsigned>(decision.votes_required),
             decision.hold_elapsed_ms,
             decision.hold_required_ms,
             gesture_reason_name(decision.reason),
             decision.action_id,
             std::max<int64_t>(0, result.finished_ms - result.started_ms),
             std::max<int64_t>(0, result.finished_ms - result.captured_ms),
             result_interval_ms,
             static_cast<unsigned>(observation.hand_count),
             static_cast<double>(observation.point_horizontal_ratio * 100.0F),
             observation.point_zone,
             static_cast<double>(observation.point_margin * 100.0F));
}

void start_recording(SdStorage &storage,
                     SessionState &state,
                     bool &stopping,
                     int64_t &next_photo_ms,
                     int64_t current_ms)
{
    const bool session_ready = storage.start_session(current_ms);
    if (!session_ready) {
        ESP_LOGW(TAG, "recording started without SD photo storage");
    }
    state = SessionState::RECORDING;
    stopping = false;
    next_photo_ms = current_ms + vision_config::kPhotoIntervalMs;
}

void apply_p4_practice_state(P4PracticeState p4_state,
                             SdStorage &storage,
                             SessionState &state,
                             bool &stopping,
                             int64_t &next_photo_ms,
                             int64_t current_ms)
{
    switch (p4_state) {
    case P4PracticeState::PLAYING:
        if (stopping) {
            ESP_LOGW(TAG, "P4 resumed before the previous SD session finished");
            return;
        }
        if (storage.session_active()) {
            state = SessionState::RECORDING;
            next_photo_ms = current_ms + vision_config::kPhotoIntervalMs;
        } else {
            start_recording(storage, state, stopping,
                            next_photo_ms, current_ms);
        }
        break;
    case P4PracticeState::PAUSED:
        if (state == SessionState::RECORDING) {
            state = SessionState::PAUSED;
            ESP_LOGI(TAG, "SD capture paused; current session remains open");
        }
        break;
    case P4PracticeState::FINISHED:
        if (storage.session_active()) {
            stopping = true;
        }
        state = SessionState::FINISHED;
        break;
    default:
        break;
    }
}
} // namespace

extern "C" void app_main(void)
{
    SessionState state = SessionState::IDLE;
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM is required but was not initialized");
        state = SessionState::ERROR;
    }

    CameraDriver camera;
    if (state != SessionState::ERROR && camera.init() != ESP_OK) {
        state = SessionState::ERROR;
    }

    HandAi hand_ai;
    if (state != SessionState::ERROR && !hand_ai.init()) {
        state = SessionState::ERROR;
    }

    AiWorker ai_worker;
    if (state != SessionState::ERROR && !ai_worker.init(hand_ai)) {
        state = SessionState::ERROR;
    }

    SdStorage storage;
    const esp_err_t sd_result = storage.init();
    if (sd_result != ESP_OK) {
        ESP_LOGW(TAG, "continuing with gesture recognition only");
    }

    if (state == SessionState::ERROR) {
        ESP_LOGE(TAG, "core vision initialization failed; entering ERROR state");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    const esp_err_t p4_link_result = p4_control_link_init();
    if (p4_link_result != ESP_OK) {
        ESP_LOGW(TAG, "continuing without P4 control link: %s",
                 esp_err_to_name(p4_link_result));
    }

    GestureEngine gesture_engine;
    bool stopping = false;
    int64_t current_ms = now_ms();
    int64_t next_frame_ms = current_ms;
    int64_t next_ai_ms = current_ms;
    int64_t next_photo_ms = current_ms + vision_config::kPhotoIntervalMs;
    int64_t next_memory_log_ms = current_ms + vision_config::kMemoryLogIntervalMs;
    int64_t last_capture_warning_ms = -vision_config::kCaptureWarningIntervalMs;
    int64_t last_ai_finished_ms = 0;

    ESP_LOGI(TAG, "ready; state=IDLE, web=disabled, controls=custom_point_direction");
    while (true) {
        current_ms = now_ms();

        if (stopping && storage.writer_idle()) {
            const bool score_requested = state == SessionState::SCORE_REQUESTED;
            storage.finish_session(current_ms);
            stopping = false;
            state = score_requested ? SessionState::SCORE_REQUESTED
                                    : SessionState::FINISHED;
            ESP_LOGI(TAG, "state=%s", session_state_name(state));
        }

        AiInferenceResult inference_result;
        while (ai_worker.receive(inference_result)) {
            GestureDecision decision;
            const SessionState decision_state = stopping ? SessionState::ERROR : state;
            const int64_t received_ms = now_ms();
            const int64_t result_interval_ms = last_ai_finished_ms == 0
                                                   ? 0
                                                   : received_ms - last_ai_finished_ms;
            last_ai_finished_ms = received_ms;
            if (inference_result.valid) {
                decision = gesture_engine.update(inference_result.observation, received_ms);
                log_ai_decision(inference_result, decision, decision_state, result_interval_ms);
                if (decision.action_id != 0) {
                    (void)emit_action(decision.action_id);
                }
            }
        }

        P4ControlAck ack;
        while (p4_control_link_receive_ack(ack)) {
            ESP_LOGD(TAG, "P4 ACK consumed: command=%u handled=%d",
                     static_cast<unsigned>(ack.command), ack.handled);
        }

        P4PracticeState p4_practice_state;
        while (p4_control_link_receive_practice_state(p4_practice_state)) {
            apply_p4_practice_state(p4_practice_state, storage, state,
                                    stopping, next_photo_ms, now_ms());
        }

        current_ms = now_ms();
        if (current_ms >= next_memory_log_ms) {
            log_memory();
            advance_periodic_deadline(next_memory_log_ms, vision_config::kMemoryLogIntervalMs, current_ms);
        }

        if (current_ms < next_frame_ms) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        advance_periodic_deadline(next_frame_ms, vision_config::kCaptureFrameIntervalMs, current_ms);

        camera_fb_t *frame = camera.acquire_frame();
        if (frame == nullptr) {
            if (current_ms - last_capture_warning_ms >= vision_config::kCaptureWarningIntervalMs) {
                ESP_LOGW(TAG, "camera frame unavailable; skipping current cycle");
                last_capture_warning_ms = current_ms;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        const int64_t captured_ms = now_ms();
        if (captured_ms >= next_ai_ms &&
            ai_worker.submit_jpeg(frame->buf,
                                  frame->len,
                                  static_cast<uint16_t>(frame->width),
                                  static_cast<uint16_t>(frame->height),
                                  captured_ms)) {
            advance_periodic_deadline(next_ai_ms, vision_config::kAiIntervalMs, captured_ms);
        }

        const bool photo_due =
            state == SessionState::RECORDING && !stopping && captured_ms >= next_photo_ms;
        uint8_t *jpeg_copy = nullptr;
        size_t jpeg_size = 0;
        if (photo_due && storage.can_accept_photo() && frame->format == PIXFORMAT_JPEG && frame->buf != nullptr &&
            frame->len > 0) {
            jpeg_copy = static_cast<uint8_t *>(
                heap_caps_malloc(frame->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (jpeg_copy != nullptr) {
                jpeg_size = frame->len;
                std::memcpy(jpeg_copy, frame->buf, frame->len);
            } else {
                ESP_LOGW(TAG, "PSRAM allocation failed; dropping current JPEG");
            }
        }

        camera.return_frame(frame);

        if (photo_due) {
            advance_periodic_deadline(next_photo_ms, vision_config::kPhotoIntervalMs, captured_ms);
            if (jpeg_copy != nullptr) {
                if (state == SessionState::RECORDING && !stopping) {
                    storage.enqueue_jpeg(jpeg_copy, jpeg_size);
                } else {
                    heap_caps_free(jpeg_copy);
                }
            }
        }

        vTaskDelay(1);
    }
}
