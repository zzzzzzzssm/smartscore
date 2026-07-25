#include "network_provisioning_internal.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAX_OBSERVERS 4

typedef struct {
    network_status_observer_t callback;
    void *context;
} observer_slot_t;

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static network_status_t s_status;
static observer_slot_t s_observers[MAX_OBSERVERS];

void network_state_machine_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&s_status, 0, sizeof(s_status));
    memset(s_observers, 0, sizeof(s_observers));
    s_status.state = NETWORK_STATE_UNINITIALIZED;
    strlcpy(s_status.ip, "0.0.0.0", sizeof(s_status.ip));
    xSemaphoreGive(s_mutex);
}

void network_state_transition(network_state_t state,
                              int request_id,
                              unsigned retry_count,
                              const char *ssid,
                              const char *ip,
                              const char *failure_reason,
                              bool credentials_saved)
{
    network_status_t snapshot;
    observer_slot_t observers[MAX_OBSERVERS];

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.state = state;
    s_status.request_id = request_id;
    s_status.retry_count = retry_count;
    s_status.credentials_saved = credentials_saved;
    strlcpy(s_status.ssid, ssid != NULL ? ssid : "", sizeof(s_status.ssid));
    strlcpy(s_status.ip, ip != NULL && ip[0] != '\0' ? ip : "0.0.0.0",
            sizeof(s_status.ip));
    strlcpy(s_status.failure_reason,
            failure_reason != NULL ? failure_reason : "",
            sizeof(s_status.failure_reason));
    snapshot = s_status;
    memcpy(observers, s_observers, sizeof(observers));
    xSemaphoreGive(s_mutex);

    for (size_t index = 0; index < MAX_OBSERVERS; ++index) {
        if (observers[index].callback != NULL) {
            observers[index].callback(&snapshot, observers[index].context);
        }
    }
}

network_status_t network_state_snapshot(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    network_status_t snapshot = s_status;
    xSemaphoreGive(s_mutex);
    return snapshot;
}

esp_err_t network_state_add_observer(network_status_observer_t callback,
                                     void *context)
{
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t index = 0; index < MAX_OBSERVERS; ++index) {
        if (s_observers[index].callback == callback &&
            s_observers[index].context == context) {
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    for (size_t index = 0; index < MAX_OBSERVERS; ++index) {
        if (s_observers[index].callback == NULL) {
            s_observers[index].callback = callback;
            s_observers[index].context = context;
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NO_MEM;
}

void network_state_remove_observer(network_status_observer_t callback,
                                   void *context)
{
    if (s_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t index = 0; index < MAX_OBSERVERS; ++index) {
        if (s_observers[index].callback == callback &&
            s_observers[index].context == context) {
            memset(&s_observers[index], 0, sizeof(s_observers[index]));
        }
    }
    xSemaphoreGive(s_mutex);
}
