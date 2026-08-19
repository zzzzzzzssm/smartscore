#include "practice_advice_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deepseek_advice.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network_provisioning.h"
#include "practice_advice_state.h"

#define PRACTICE_ADVICE_TASK_STACK_BYTES 12288
#define PRACTICE_ADVICE_TASK_PRIORITY 3

typedef struct {
    uint32_t generation;
    char *score_json;
    size_t score_length;
} practice_advice_job_t;

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t worker;
    practice_advice_core_state_t core;
    uint32_t boot_id;
    uint32_t session_counter;
    char session_id[PRACTICE_ADVICE_SESSION_ID_CAPACITY];
    char error[PRACTICE_ADVICE_ERROR_CAPACITY];
    char message[PRACTICE_ADVICE_MESSAGE_CAPACITY];
    char *advice_json;
    size_t advice_length;
    practice_advice_job_t *pending_job;
    bool initialized;
} practice_advice_service_t;

static const char *TAG = "PRACTICE_ADVICE";
static practice_advice_service_t s_service;

static practice_advice_state_t public_state(
    practice_advice_core_phase_t phase)
{
    switch (phase) {
    case PRACTICE_ADVICE_CORE_WAITING_SCORE:
        return PRACTICE_ADVICE_WAITING_SCORE;
    case PRACTICE_ADVICE_CORE_RUNNING:
        return PRACTICE_ADVICE_RUNNING;
    case PRACTICE_ADVICE_CORE_READY:
        return PRACTICE_ADVICE_READY;
    case PRACTICE_ADVICE_CORE_SKIPPED_OFFLINE:
        return PRACTICE_ADVICE_SKIPPED_OFFLINE;
    case PRACTICE_ADVICE_CORE_FAILED:
        return PRACTICE_ADVICE_FAILED;
    case PRACTICE_ADVICE_CORE_NONE:
    default:
        return PRACTICE_ADVICE_NONE;
    }
}

const char *practice_advice_state_name(practice_advice_state_t state)
{
    switch (state) {
    case PRACTICE_ADVICE_WAITING_SCORE: return "waiting_score";
    case PRACTICE_ADVICE_RUNNING: return "running";
    case PRACTICE_ADVICE_READY: return "ready";
    case PRACTICE_ADVICE_SKIPPED_OFFLINE: return "skipped_offline";
    case PRACTICE_ADVICE_FAILED: return "failed";
    case PRACTICE_ADVICE_NONE:
    default: return "none";
    }
}

static char *allocate_copy(const char *source, size_t length)
{
    if (source == NULL || length == 0) return NULL;
    char *copy = NULL;
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        copy = heap_caps_malloc(length + 1U,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (copy == NULL) {
        copy = heap_caps_malloc(length + 1U,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (copy == NULL) return NULL;
    memcpy(copy, source, length);
    copy[length] = '\0';
    return copy;
}

static void free_job(practice_advice_job_t *job)
{
    if (job == NULL) return;
    heap_caps_free(job->score_json);
    free(job);
}

static void set_terminal_locked(uint32_t generation,
                                practice_advice_core_phase_t phase,
                                const char *error,
                                const char *message)
{
    if (!practice_advice_state_finish(&s_service.core, generation, phase)) {
        return;
    }
    strlcpy(s_service.error, error != NULL ? error : "",
            sizeof(s_service.error));
    strlcpy(s_service.message, message != NULL ? message : "",
            sizeof(s_service.message));
}

static const char *error_message(deepseek_advice_error_t error)
{
    switch (error) {
    case DEEPSEEK_ADVICE_ERR_NOT_CONFIGURED:
        return "设备未配置练习建议服务";
    case DEEPSEEK_ADVICE_ERR_NO_MEMORY:
        return "设备内存不足，未能生成建议";
    case DEEPSEEK_ADVICE_ERR_TIMEOUT:
        return "练习建议生成超时";
    case DEEPSEEK_ADVICE_ERR_NETWORK:
        return "生成建议时网络连接中断";
    case DEEPSEEK_ADVICE_ERR_HTTP_STATUS:
        return "练习建议云服务暂时不可用";
    case DEEPSEEK_ADVICE_ERR_RESPONSE_TOO_LARGE:
    case DEEPSEEK_ADVICE_ERR_OUTER_JSON:
    case DEEPSEEK_ADVICE_ERR_INNER_JSON:
    case DEEPSEEK_ADVICE_ERR_FINISH_REASON:
    case DEEPSEEK_ADVICE_ERR_CONTENT:
    case DEEPSEEK_ADVICE_ERR_SCHEMA:
        return "练习建议返回内容异常";
    default:
        return "练习建议生成失败";
    }
}

static void practice_advice_worker(void *argument)
{
    (void)argument;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (true) {
            xSemaphoreTake(s_service.lock, portMAX_DELAY);
            practice_advice_job_t *job = s_service.pending_job;
            s_service.pending_job = NULL;
            xSemaphoreGive(s_service.lock);
            if (job == NULL) break;

            char *advice_json = NULL;
            deepseek_advice_error_t result = deepseek_advice_generate(
                job->score_json, &advice_json);
            heap_caps_free(job->score_json);
            job->score_json = NULL;

            xSemaphoreTake(s_service.lock, portMAX_DELAY);
            if (practice_advice_state_is_current(&s_service.core,
                                                 job->generation)) {
                if (result == DEEPSEEK_ADVICE_OK && advice_json != NULL &&
                    practice_advice_state_finish(
                        &s_service.core, job->generation,
                        PRACTICE_ADVICE_CORE_READY)) {
                    free(s_service.advice_json);
                    s_service.advice_json = advice_json;
                    s_service.advice_length = strlen(advice_json);
                    s_service.error[0] = '\0';
                    strlcpy(s_service.message, "练习建议已生成",
                            sizeof(s_service.message));
                    advice_json = NULL;
                } else {
                    set_terminal_locked(job->generation,
                                        PRACTICE_ADVICE_CORE_FAILED,
                                        deepseek_advice_error_name(result),
                                        error_message(result));
                }
            }
            xSemaphoreGive(s_service.lock);

            free(advice_json);
            free(job);
        }
    }
}

esp_err_t practice_advice_service_init(void)
{
    if (s_service.initialized) return ESP_OK;
    memset(&s_service, 0, sizeof(s_service));
    s_service.lock = xSemaphoreCreateMutex();
    if (s_service.lock == NULL) return ESP_ERR_NO_MEM;

    practice_advice_state_init(&s_service.core);
    s_service.boot_id = esp_random();
    if (xTaskCreate(practice_advice_worker, "practice_advice",
                    PRACTICE_ADVICE_TASK_STACK_BYTES, NULL,
                    PRACTICE_ADVICE_TASK_PRIORITY,
                    &s_service.worker) != pdPASS) {
        vSemaphoreDelete(s_service.lock);
        memset(&s_service, 0, sizeof(s_service));
        return ESP_ERR_NO_MEM;
    }
    s_service.initialized = true;
    ESP_LOGI(TAG, "background advice service ready");
    return ESP_OK;
}

esp_err_t practice_advice_service_begin_session(char *out_session_id,
                                                size_t capacity)
{
    if (!s_service.initialized) return ESP_ERR_INVALID_STATE;
    if (out_session_id != NULL && capacity == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    uint32_t generation = practice_advice_state_begin(&s_service.core);
    ++s_service.session_counter;
    if (s_service.session_counter == 0) ++s_service.session_counter;
    snprintf(s_service.session_id, sizeof(s_service.session_id),
             "boot-%08lx-practice-%08lx",
             (unsigned long)s_service.boot_id,
             (unsigned long)s_service.session_counter);
    s_service.error[0] = '\0';
    strlcpy(s_service.message, "等待本地评分完成",
            sizeof(s_service.message));
    free(s_service.advice_json);
    s_service.advice_json = NULL;
    s_service.advice_length = 0;
    practice_advice_job_t *old_pending = s_service.pending_job;
    s_service.pending_job = NULL;
    if (out_session_id != NULL) {
        strlcpy(out_session_id, s_service.session_id, capacity);
    }
    xSemaphoreGive(s_service.lock);
    free_job(old_pending);
    ESP_LOGI(TAG, "advice session started generation=%lu id=%s",
             (unsigned long)generation, s_service.session_id);
    return ESP_OK;
}

esp_err_t practice_advice_service_submit_score(const char *score_json,
                                               size_t length)
{
    if (!s_service.initialized || score_json == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (s_service.core.phase != PRACTICE_ADVICE_CORE_WAITING_SCORE) {
        xSemaphoreGive(s_service.lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t generation = s_service.core.generation;
    xSemaphoreGive(s_service.lock);

    network_status_t network = network_provisioning_get_status();
    if (network.state != NETWORK_STATE_WIFI_CONNECTED) {
        xSemaphoreTake(s_service.lock, portMAX_DELAY);
        set_terminal_locked(generation,
                            PRACTICE_ADVICE_CORE_SKIPPED_OFFLINE,
                            "advice_skipped_offline",
                            "本次练习结束时设备未联网，未生成建议");
        xSemaphoreGive(s_service.lock);
        return ESP_ERR_INVALID_STATE;
    }

    if (!deepseek_advice_api_key_configured()) {
        xSemaphoreTake(s_service.lock, portMAX_DELAY);
        set_terminal_locked(generation, PRACTICE_ADVICE_CORE_FAILED,
                            "deepseek_key_missing",
                            "设备未配置练习建议服务");
        xSemaphoreGive(s_service.lock);
        return ESP_ERR_NOT_SUPPORTED;
    }

    practice_advice_job_t *job = calloc(1, sizeof(*job));
    char *score_copy = allocate_copy(score_json, length);
    if (job == NULL || score_copy == NULL) {
        free(job);
        heap_caps_free(score_copy);
        xSemaphoreTake(s_service.lock, portMAX_DELAY);
        set_terminal_locked(generation, PRACTICE_ADVICE_CORE_FAILED,
                            "no_memory",
                            "设备内存不足，未能生成建议");
        xSemaphoreGive(s_service.lock);
        return ESP_ERR_NO_MEM;
    }
    job->generation = generation;
    job->score_json = score_copy;
    job->score_length = length;

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (!practice_advice_state_mark_running(&s_service.core, generation)) {
        xSemaphoreGive(s_service.lock);
        free_job(job);
        return ESP_ERR_INVALID_STATE;
    }
    practice_advice_job_t *old_pending = s_service.pending_job;
    s_service.pending_job = job;
    s_service.error[0] = '\0';
    strlcpy(s_service.message, "建议正在后台生成，请稍等",
            sizeof(s_service.message));
    xSemaphoreGive(s_service.lock);
    free_job(old_pending);
    xTaskNotifyGive(s_service.worker);
    return ESP_OK;
}

void practice_advice_service_score_failed(const char *error,
                                          const char *message)
{
    if (!s_service.initialized) return;
    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    set_terminal_locked(s_service.core.generation,
                        PRACTICE_ADVICE_CORE_FAILED,
                        error != NULL ? error : "score_result_not_available",
                        message != NULL ? message : "本地评分失败，未生成建议");
    xSemaphoreGive(s_service.lock);
}

void practice_advice_service_get_status(practice_advice_status_t *status)
{
    if (status == NULL) return;
    memset(status, 0, sizeof(*status));
    if (!s_service.initialized || s_service.lock == NULL) return;

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    status->initialized = true;
    status->state = public_state(s_service.core.phase);
    status->generation = s_service.core.generation;
    strlcpy(status->session_id, s_service.session_id,
            sizeof(status->session_id));
    strlcpy(status->error, s_service.error, sizeof(status->error));
    strlcpy(status->message, s_service.message, sizeof(status->message));
    status->advice_length = s_service.advice_length;
    xSemaphoreGive(s_service.lock);
}

esp_err_t practice_advice_service_copy_advice(char **out_json,
                                              size_t *out_length)
{
    if (out_json == NULL) return ESP_ERR_INVALID_ARG;
    *out_json = NULL;
    if (out_length != NULL) *out_length = 0;
    if (!s_service.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_service.lock, portMAX_DELAY);
    if (s_service.core.phase != PRACTICE_ADVICE_CORE_READY ||
        s_service.advice_json == NULL || s_service.advice_length == 0) {
        xSemaphoreGive(s_service.lock);
        return ESP_ERR_NOT_FOUND;
    }
    char *copy = malloc(s_service.advice_length + 1U);
    if (copy != NULL) {
        memcpy(copy, s_service.advice_json, s_service.advice_length + 1U);
    }
    size_t length = s_service.advice_length;
    xSemaphoreGive(s_service.lock);
    if (copy == NULL) return ESP_ERR_NO_MEM;
    *out_json = copy;
    if (out_length != NULL) *out_length = length;
    return ESP_OK;
}
