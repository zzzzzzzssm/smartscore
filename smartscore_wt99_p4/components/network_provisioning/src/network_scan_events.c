#include "network_provisioning_internal.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAX_SCAN_OBSERVERS 4

typedef struct {
    network_scan_observer_t callback;
    void *context;
} scan_observer_slot_t;

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static scan_observer_slot_t s_observers[MAX_SCAN_OBSERVERS];

void network_scan_events_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_observers, 0, sizeof(s_observers));
    xSemaphoreGive(s_mutex);
}

esp_err_t network_scan_events_add_observer(network_scan_observer_t callback,
                                           void *context)
{
    if (callback == NULL || s_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t index = 0; index < MAX_SCAN_OBSERVERS; ++index) {
        if (s_observers[index].callback == callback &&
            s_observers[index].context == context) {
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    for (size_t index = 0; index < MAX_SCAN_OBSERVERS; ++index) {
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

void network_scan_events_remove_observer(network_scan_observer_t callback,
                                         void *context)
{
    if (s_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t index = 0; index < MAX_SCAN_OBSERVERS; ++index) {
        if (s_observers[index].callback == callback &&
            s_observers[index].context == context) {
            memset(&s_observers[index], 0, sizeof(s_observers[index]));
        }
    }
    xSemaphoreGive(s_mutex);
}

esp_err_t network_scan_events_publish(const network_scan_event_t *event)
{
    if (event == NULL || s_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    scan_observer_slot_t observers[MAX_SCAN_OBSERVERS];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(observers, s_observers, sizeof(observers));
    xSemaphoreGive(s_mutex);

    esp_err_t first_error = ESP_OK;
    for (size_t index = 0; index < MAX_SCAN_OBSERVERS; ++index) {
        if (observers[index].callback == NULL) {
            continue;
        }
        esp_err_t err = observers[index].callback(event,
                                                  observers[index].context);
        if (first_error == ESP_OK && err != ESP_OK) {
            first_error = err;
        }
    }
    return first_error;
}
