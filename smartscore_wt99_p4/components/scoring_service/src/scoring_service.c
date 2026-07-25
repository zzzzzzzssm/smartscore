#include "scoring_service.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "input_source_manager.h"
#include "performance_recorder.h"
#include "score_data.h"
#include "score_engine.h"

#define SCORING_TASK_STACK_BYTES 8192
#define SCORING_TASK_PRIORITY 4
#define SCORING_COMPLETE_BIT BIT0

typedef struct {
    score_document_t score;
    performance_snapshot_t performance;
} scoring_job_t;

typedef struct {
    SemaphoreHandle_t lock;
    QueueHandle_t queue;
    EventGroupHandle_t events;
    TaskHandle_t task;
    scoring_service_status_t status;
    char *result_json;
    size_t result_length;
} scoring_service_t;

static const char *TAG = "SCORING_SERVICE";
static scoring_service_t s_service;

const char *scoring_service_state_name(scoring_service_state_t state)
{
    switch (state) {
        case SCORING_SERVICE_IDLE:
            return "idle";
        case SCORING_SERVICE_RECORDING:
            return "recording";
        case SCORING_SERVICE_PAUSED:
            return "paused";
        case SCORING_SERVICE_SCORING:
            return "scoring";
        case SCORING_SERVICE_READY:
            return "ready";
        case SCORING_SERVICE_ERROR:
            return "error";
        case SCORING_SERVICE_UNINITIALIZED:
        default:
            return "uninitialized";
    }
}

static void set_error_locked(esp_err_t error,
                             const char *code,
                             const char *message)
{
    s_service.status.state = SCORING_SERVICE_ERROR;
    s_service.status.last_error = error;
    strlcpy(s_service.status.error,
            code != NULL ? code : esp_err_to_name(error),
            sizeof(s_service.status.error));
    strlcpy(s_service.status.message,
            message != NULL ? message : s_service.status.error,
            sizeof(s_service.status.message));
}

static void scoring_task(void *argument)
{
    (void)argument;
    scoring_job_t job;
    while (true) {
        if (xQueueReceive(s_service.queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        size_t target_count = job.score.note_count;
        size_t played_count = job.performance.count;
        char *result_json = NULL;
        size_t result_length = 0;
        char engine_error[64] = {0};
        esp_err_t err = score_engine_build_midi_result_json(
            &job.score, &job.performance, &result_json, &result_length,
            engine_error, sizeof(engine_error));
        score_document_release(&job.score);
        performance_snapshot_release(&job.performance);

        xSemaphoreTake(s_service.lock, portMAX_DELAY);
        s_service.status.target_count = target_count;
        s_service.status.played_count = played_count;
        if (err == ESP_OK) {
            score_engine_result_free(s_service.result_json);
            s_service.result_json = result_json;
            s_service.result_length = result_length;
            s_service.status.state = SCORING_SERVICE_READY;
            s_service.status.last_error = ESP_OK;
            s_service.status.error[0] = '\0';
            strlcpy(s_service.status.message, "scoring complete",
                    sizeof(s_service.status.message));
            ESP_LOGI(TAG, "scoring complete: target=%u played=%u result=%u bytes",
                     (unsigned)target_count, (unsigned)played_count,
                     (unsigned)result_length);
        } else {
            score_engine_result_free(result_json);
            set_error_locked(err,
                             engine_error[0] != '\0'
                                 ? engine_error
                                 : "score_engine_failed",
                             "unable to build complete scoring result");
            ESP_LOGE(TAG, "scoring failed: %s", s_service.status.error);
        }
        xSemaphoreGive(s_service.lock);
        input_source_manager_unlock();
        xEventGroupSetBits(s_service.events, SCORING_COMPLETE_BIT);
    }
}

esp_err_t scoring_service_init(void)
{
    if (s_service.lock != NULL) {
        return ESP_OK;
    }
    memset(&s_service, 0, sizeof(s_service));
    s_service.lock = xSemaphoreCreateMutex();
    s_service.queue = xQueueCreate(1, sizeof(scoring_job_t));
    s_service.events = xEventGroupCreate();
    if (s_service.lock == NULL || s_service.queue == NULL ||
        s_service.events == NULL) {
        if (s_service.lock != NULL) {
            vSemaphoreDelete(s_service.lock);
        }
        if (s_service.queue != NULL) {
            vQueueDelete(s_service.queue);
        }
        if (s_service.events != NULL) {
            vEventGroupDelete(s_service.events);
        }
        memset(&s_service, 0, sizeof(s_service));
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(scoring_task, "score_task", SCORING_TASK_STACK_BYTES,
                    NULL, SCORING_TASK_PRIORITY, &s_service.task) != pdPASS) {
        vSemaphoreDelete(s_service.lock);
        vQueueDelete(s_service.queue);
        vEventGroupDelete(s_service.events);
        memset(&s_service, 0, sizeof(s_service));
        return ESP_ERR_NO_MEM;
    }
    input_source_status_t input_status;
    input_source_manager_get_status(&input_status);
    s_service.status.state = SCORING_SERVICE_IDLE;
    s_service.status.input_source = input_status.selected_input;
    s_service.status.last_error = ESP_OK;
    strlcpy(s_service.status.profile, "midi_strict",
            sizeof(s_service.status.profile));
    return ESP_OK;
}

esp_err_t scoring_service_start(const char *input_source,
                                const char *profile)
{
    const char *profile_value = profile != NULL ? profile : "midi_strict";
    if (s_service.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    input_source_status_t input_status;
    input_source_manager_get_status(&input_status);
    input_source_t requested_source = input_status.selected_input;
    if (input_source != NULL &&
        (!input_source_from_name(input_source, &requested_source) ||
         requested_source != input_status.selected_input)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(profile_value, "midi_strict") != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    score_data_status_t score;
    score_data_get_status(&score);
    if (!score.loaded) {
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (s_service.status.state == SCORING_SERVICE_RECORDING ||
        s_service.status.state == SCORING_SERVICE_PAUSED ||
        s_service.status.state == SCORING_SERVICE_SCORING) {
        xSemaphoreGive(s_service.lock);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_service.lock);

    input_source_t locked_source = INPUT_SOURCE_NONE;
    esp_err_t err = input_source_manager_lock(&locked_source);
    if (err != ESP_OK) {
        return err;
    }
    if (locked_source != INPUT_SOURCE_USB_MIDI &&
        locked_source != INPUT_SOURCE_AUDIO_S3) {
        input_source_manager_unlock();
        return ESP_ERR_NOT_SUPPORTED;
    }

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    err = performance_recorder_start(locked_source);
    if (err != ESP_OK) {
        set_error_locked(err, "performance_recorder_start_failed",
                         "unable to allocate the performance buffer");
        xSemaphoreGive(s_service.lock);
        input_source_manager_unlock();
        return err;
    }
    score_engine_result_free(s_service.result_json);
    s_service.result_json = NULL;
    s_service.result_length = 0;
    xEventGroupClearBits(s_service.events, SCORING_COMPLETE_BIT);
    s_service.status.state = SCORING_SERVICE_RECORDING;
    s_service.status.input_source = locked_source;
    s_service.status.target_count = score.note_count;
    s_service.status.played_count = 0;
    s_service.status.last_error = ESP_OK;
    s_service.status.error[0] = '\0';
    strlcpy(s_service.status.message, "recording note performance",
            sizeof(s_service.status.message));
    strlcpy(s_service.status.profile, profile_value,
            sizeof(s_service.status.profile));
    xSemaphoreGive(s_service.lock);
    ESP_LOGI(TAG, "practice started: target=%u source=%s profile=%s",
             (unsigned)score.note_count,
             input_source_name(locked_source), profile_value);
    return ESP_OK;
}

esp_err_t scoring_service_pause(void)
{
    if (s_service.lock == NULL) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (s_service.status.state != SCORING_SERVICE_RECORDING) {
        xSemaphoreGive(s_service.lock);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_service.lock);

    esp_err_t err = performance_recorder_pause();
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (s_service.status.state == SCORING_SERVICE_RECORDING) {
        s_service.status.state = SCORING_SERVICE_PAUSED;
        strlcpy(s_service.status.message, "performance paused",
                sizeof(s_service.status.message));
    }
    xSemaphoreGive(s_service.lock);
    return ESP_OK;
}

esp_err_t scoring_service_resume(void)
{
    if (s_service.lock == NULL) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (s_service.status.state != SCORING_SERVICE_PAUSED) {
        xSemaphoreGive(s_service.lock);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_service.lock);

    esp_err_t err = performance_recorder_resume();
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (s_service.status.state == SCORING_SERVICE_PAUSED) {
        s_service.status.state = SCORING_SERVICE_RECORDING;
        strlcpy(s_service.status.message, "recording note performance",
                sizeof(s_service.status.message));
    }
    xSemaphoreGive(s_service.lock);
    return ESP_OK;
}

esp_err_t scoring_service_wait_for_result(uint32_t timeout_ms)
{
    if (s_service.lock == NULL || s_service.events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    scoring_service_state_t state = s_service.status.state;
    esp_err_t current_error = s_service.status.last_error;
    xSemaphoreGive(s_service.lock);
    if (state == SCORING_SERVICE_READY) {
        return ESP_OK;
    }
    if (state == SCORING_SERVICE_ERROR) {
        return current_error != ESP_OK ? current_error : ESP_FAIL;
    }
    if (state != SCORING_SERVICE_SCORING) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_service.events, SCORING_COMPLETE_BIT,
        pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    if ((bits & SCORING_COMPLETE_BIT) == 0U) {
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    state = s_service.status.state;
    current_error = s_service.status.last_error;
    xSemaphoreGive(s_service.lock);
    if (state == SCORING_SERVICE_READY) {
        return ESP_OK;
    }
    return current_error != ESP_OK ? current_error : ESP_FAIL;
}

esp_err_t scoring_service_with_result(scoring_result_consumer_t consumer,
                                      void *context)
{
    if (consumer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_service.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (s_service.status.state != SCORING_SERVICE_READY ||
        s_service.result_json == NULL) {
        esp_err_t err = s_service.status.last_error != ESP_OK
                            ? s_service.status.last_error
                            : ESP_ERR_INVALID_STATE;
        xSemaphoreGive(s_service.lock);
        return err;
    }
    esp_err_t err = consumer(s_service.result_json,
                             s_service.result_length, context);
    xSemaphoreGive(s_service.lock);
    return err;
}

esp_err_t scoring_service_stop(void)
{
    if (s_service.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (s_service.status.state != SCORING_SERVICE_RECORDING &&
        s_service.status.state != SCORING_SERVICE_PAUSED) {
        esp_err_t err = s_service.status.last_error != ESP_OK
                            ? s_service.status.last_error
                            : ESP_ERR_INVALID_STATE;
        xSemaphoreGive(s_service.lock);
        return err;
    }
    xSemaphoreGive(s_service.lock);

    scoring_job_t job = {0};
    esp_err_t err = performance_recorder_stop_and_take_snapshot(
        &job.performance);
    if (err == ESP_OK) {
        err = score_data_copy(&job.score);
    }
    if (err != ESP_OK) {
        score_document_release(&job.score);
        performance_snapshot_release(&job.performance);
        xSemaphoreTake(s_service.lock, portMAX_DELAY);
        performance_recorder_status_t recorder;
        performance_recorder_get_status(&recorder);
        set_error_locked(err,
                         recorder.error[0] != '\0'
                             ? recorder.error
                             : "scoring_snapshot_failed",
                         recorder.message[0] != '\0'
                             ? recorder.message
                             : "unable to create scoring snapshot");
        xSemaphoreGive(s_service.lock);
        input_source_manager_unlock();
        return err;
    }

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    s_service.status.state = SCORING_SERVICE_SCORING;
    s_service.status.played_count = job.performance.count;
    strlcpy(s_service.status.message, "scoring started",
            sizeof(s_service.status.message));
    xSemaphoreGive(s_service.lock);
    if (xQueueSend(s_service.queue, &job, 0) != pdTRUE) {
        score_document_release(&job.score);
        performance_snapshot_release(&job.performance);
        xSemaphoreTake(s_service.lock, portMAX_DELAY);
        set_error_locked(ESP_ERR_TIMEOUT, "scoring_queue_busy",
                         "scoring task is busy");
        xSemaphoreGive(s_service.lock);
        input_source_manager_unlock();
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t scoring_service_reset(void)
{
    if (s_service.lock == NULL || s_service.events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (s_service.status.state == SCORING_SERVICE_RECORDING ||
        s_service.status.state == SCORING_SERVICE_PAUSED ||
        s_service.status.state == SCORING_SERVICE_SCORING) {
        xSemaphoreGive(s_service.lock);
        return ESP_ERR_INVALID_STATE;
    }
    score_engine_result_free(s_service.result_json);
    s_service.result_json = NULL;
    s_service.result_length = 0;
    xEventGroupClearBits(s_service.events, SCORING_COMPLETE_BIT);
    s_service.status.state = SCORING_SERVICE_IDLE;
    s_service.status.target_count = 0;
    s_service.status.played_count = 0;
    s_service.status.last_error = ESP_OK;
    s_service.status.error[0] = '\0';
    strlcpy(s_service.status.message, "ready for practice",
            sizeof(s_service.status.message));
    xSemaphoreGive(s_service.lock);

    input_source_manager_unlock();
    ESP_LOGI(TAG, "practice session reset");
    return ESP_OK;
}

static bool handle_note_event(input_source_t source,
                              const usb_midi_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    bool recording_source =
        s_service.status.state == SCORING_SERVICE_RECORDING &&
        s_service.status.input_source == source;
    xSemaphoreGive(s_service.lock);

    if ((event->type == USB_MIDI_EVENT_NOTE_ON ||
         event->type == USB_MIDI_EVENT_NOTE_OFF) && recording_source) {
        performance_recorder_process_midi(
            event->type == USB_MIDI_EVENT_NOTE_ON,
            event->midi, event->velocity, event->channel,
            event->timestamp_us);
        performance_recorder_status_t recorder;
        performance_recorder_get_status(&recorder);
        if (recorder.state == PERFORMANCE_RECORDER_ERROR &&
            s_service.lock != NULL) {
            xSemaphoreTake(s_service.lock, portMAX_DELAY);
            set_error_locked(recorder.last_error, recorder.error,
                             recorder.message);
            xSemaphoreGive(s_service.lock);
            input_source_manager_unlock();
        }
        return true;
    }
    return false;
}

void scoring_service_handle_usb_event(const usb_midi_event_t *event)
{
    if (handle_note_event(INPUT_SOURCE_USB_MIDI, event)) {
        return;
    }
    if (event == NULL) {
        return;
    }

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    bool active_usb =
        (s_service.status.state == SCORING_SERVICE_RECORDING ||
         s_service.status.state == SCORING_SERVICE_PAUSED) &&
        s_service.status.input_source == INPUT_SOURCE_USB_MIDI;
    xSemaphoreGive(s_service.lock);

    const char *code = NULL;
    const char *message = NULL;
    esp_err_t error = ESP_FAIL;
    if (event->type == USB_MIDI_EVENT_DISCONNECTED) {
        code = "input_interrupted";
        message = "USB MIDI input disconnected during practice";
        error = ESP_ERR_INVALID_STATE;
    } else if (event->type == USB_MIDI_EVENT_ERROR) {
        code = "usb_midi_event_queue_full";
        message = "USB MIDI event queue overflowed; recording stopped";
        error = ESP_ERR_NO_MEM;
    }
    if (code == NULL || s_service.lock == NULL) {
        return;
    }
    if (!active_usb) {
        return;
    }
    performance_recorder_abort(error, code, message);
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    set_error_locked(error, code, message);
    xSemaphoreGive(s_service.lock);
    input_source_manager_unlock();
}

void scoring_service_handle_audio_s3_event(const usb_midi_event_t *event)
{
    (void)handle_note_event(INPUT_SOURCE_AUDIO_S3, event);
}

void scoring_service_get_status(scoring_service_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }
    memset(out_status, 0, sizeof(*out_status));
    if (s_service.lock == NULL) {
        out_status->state = SCORING_SERVICE_UNINITIALIZED;
        return;
    }
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    *out_status = s_service.status;
    if (out_status->state == SCORING_SERVICE_RECORDING ||
        out_status->state == SCORING_SERVICE_PAUSED) {
        performance_recorder_status_t recorder;
        performance_recorder_get_status(&recorder);
        out_status->played_count = recorder.note_count;
    }
    xSemaphoreGive(s_service.lock);
}
