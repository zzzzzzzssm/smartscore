#include "wifi_remote.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static wifi_remote_status_t s_status;
static wifi_remote_capabilities_t s_capabilities;

static void ensure_mutex(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    }
}

void wifi_remote_status_reset(void)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&s_status, 0, sizeof(s_status));
    memset(&s_capabilities, 0, sizeof(s_capabilities));
    s_status.mode = WIFI_REMOTE_MODE_STOPPED;
    strlcpy(s_status.ip, "0.0.0.0", sizeof(s_status.ip));
    strlcpy(s_capabilities.coprocessor_name, "unknown",
            sizeof(s_capabilities.coprocessor_name));
    strlcpy(s_capabilities.coprocessor_firmware, "unknown",
            sizeof(s_capabilities.coprocessor_firmware));
    xSemaphoreGive(s_mutex);
}

void wifi_remote_status_set_initialized(bool initialized)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.initialized = initialized;
    xSemaphoreGive(s_mutex);
}

void wifi_remote_status_set_started(bool started, wifi_remote_mode_t mode)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.started = started;
    s_status.mode = mode;
    if (!started) {
        s_status.connected = false;
        strlcpy(s_status.ip, "0.0.0.0", sizeof(s_status.ip));
    }
    xSemaphoreGive(s_mutex);
}

void wifi_remote_status_set_connected(const char *ip)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.connected = true;
    s_status.disconnect_reason = 0;
    strlcpy(s_status.ip, ip != NULL ? ip : "0.0.0.0", sizeof(s_status.ip));
    xSemaphoreGive(s_mutex);
}

void wifi_remote_status_set_disconnected(uint8_t reason)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.connected = false;
    s_status.disconnect_reason = reason;
    strlcpy(s_status.ip, "0.0.0.0", sizeof(s_status.ip));
    xSemaphoreGive(s_mutex);
}

void wifi_remote_status_set_hosted(bool ready)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_capabilities.hosted_transport_ready = ready;
    xSemaphoreGive(s_mutex);
}

void wifi_remote_status_set_wifi_capability(bool available)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_capabilities.wifi_available = available;
    xSemaphoreGive(s_mutex);
}

void wifi_remote_set_ble_capability(bool available)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_capabilities.ble_available = available;
    xSemaphoreGive(s_mutex);
}

void wifi_remote_status_set_coprocessor(uint32_t chip_id,
                                       const char *name,
                                       const char *firmware)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_capabilities.coprocessor_chip_id = chip_id;
    strlcpy(s_capabilities.coprocessor_name, name != NULL ? name : "unknown",
            sizeof(s_capabilities.coprocessor_name));
    strlcpy(s_capabilities.coprocessor_firmware,
            firmware != NULL ? firmware : "unknown",
            sizeof(s_capabilities.coprocessor_firmware));
    xSemaphoreGive(s_mutex);
}

wifi_remote_status_t wifi_remote_get_status(void)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    wifi_remote_status_t copy = s_status;
    xSemaphoreGive(s_mutex);
    return copy;
}

wifi_remote_capabilities_t wifi_remote_get_capabilities(void)
{
    ensure_mutex();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    wifi_remote_capabilities_t copy = s_capabilities;
    xSemaphoreGive(s_mutex);
    return copy;
}
