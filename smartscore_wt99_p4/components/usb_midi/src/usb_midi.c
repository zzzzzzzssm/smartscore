#include "usb_midi.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "usb_midi_internal.h"

#define USB_MIDI_EVENT_QUEUE_LENGTH 64
#define USB_MIDI_EVENT_TASK_STACK_BYTES 4096
#define USB_MIDI_EVENT_TASK_PRIORITY 7

static const char *TAG = "USB_MIDI";
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_event_queue;
static TaskHandle_t s_event_task;
static usb_midi_status_t s_status;
static usb_midi_event_handler_t s_event_handler;
static void *s_event_handler_context;
static uint32_t s_reported_dropped_events;

static void midi_event_task(void *argument)
{
    (void)argument;
    usb_midi_event_t event;
    while (true) {
        bool received = xQueueReceive(s_event_queue, &event,
                                      pdMS_TO_TICKS(20)) == pdTRUE;

        usb_midi_event_handler_t handler;
        void *context;
        taskENTER_CRITICAL(&s_status_lock);
        handler = s_event_handler;
        context = s_event_handler_context;
        taskEXIT_CRITICAL(&s_status_lock);
        if (received && handler != NULL) {
            handler(&event, context);
        }
        taskENTER_CRITICAL(&s_status_lock);
        uint32_t dropped_events = s_status.dropped_events;
        taskEXIT_CRITICAL(&s_status_lock);
        if (dropped_events != s_reported_dropped_events) {
            s_reported_dropped_events = dropped_events;
            usb_midi_event_t error_event = {
                .type = USB_MIDI_EVENT_ERROR,
            };
            if (handler != NULL) {
                handler(&error_event, context);
            }
        }
    }
}

bool usb_midi_queue_event(const usb_midi_event_t *event)
{
    if (event == NULL || s_event_queue == NULL) {
        return false;
    }
    if (xQueueSend(s_event_queue, event, 0) == pdTRUE) {
        return true;
    }

    taskENTER_CRITICAL(&s_status_lock);
    ++s_status.dropped_events;
    s_status.last_error = ESP_ERR_NO_MEM;
    strlcpy(s_status.error, "usb_midi_event_queue_full",
            sizeof(s_status.error));
    taskEXIT_CRITICAL(&s_status_lock);
    return false;
}

void usb_midi_publish_connection(bool connected,
                                 uint16_t vid,
                                 uint16_t pid,
                                 const char *product)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_status.connected = connected;
    s_status.vid = connected ? vid : 0;
    s_status.pid = connected ? pid : 0;
    if (connected && product != NULL) {
        strlcpy(s_status.product, product, sizeof(s_status.product));
    } else {
        s_status.product[0] = '\0';
    }
    if (connected) {
        s_status.last_error = ESP_OK;
        s_status.error[0] = '\0';
    }
    taskEXIT_CRITICAL(&s_status_lock);

    usb_midi_event_t event = {
        .type = connected ? USB_MIDI_EVENT_CONNECTED
                          : USB_MIDI_EVENT_DISCONNECTED,
    };
    usb_midi_queue_event(&event);
}

void usb_midi_publish_error(esp_err_t error, const char *message)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_status.last_error = error;
    strlcpy(s_status.error, message != NULL ? message : esp_err_to_name(error),
            sizeof(s_status.error));
    taskEXIT_CRITICAL(&s_status_lock);
}

esp_err_t usb_midi_init(void)
{
    taskENTER_CRITICAL(&s_status_lock);
    bool already_initialized = s_status.initialized;
    taskEXIT_CRITICAL(&s_status_lock);
    if (already_initialized) {
        return ESP_OK;
    }

    if (s_event_queue == NULL) {
        s_event_queue = xQueueCreate(USB_MIDI_EVENT_QUEUE_LENGTH,
                                     sizeof(usb_midi_event_t));
        if (s_event_queue == NULL) {
            usb_midi_publish_error(ESP_ERR_NO_MEM,
                                   "usb_midi_queue_allocation_failed");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_event_task == NULL &&
        xTaskCreate(midi_event_task, "usb_midi_events",
                    USB_MIDI_EVENT_TASK_STACK_BYTES, NULL,
                    USB_MIDI_EVENT_TASK_PRIORITY, &s_event_task) != pdPASS) {
        usb_midi_publish_error(ESP_ERR_NO_MEM,
                               "usb_midi_task_allocation_failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = usb_midi_host_start();
    if (err != ESP_OK) {
        usb_midi_publish_error(err, "usb_midi_host_start_failed");
        return err;
    }

    taskENTER_CRITICAL(&s_status_lock);
    s_status.initialized = true;
    s_status.last_error = ESP_OK;
    s_status.error[0] = '\0';
    taskEXIT_CRITICAL(&s_status_lock);
    ESP_LOGI(TAG, "USB MIDI receive pipeline initialized");
    return ESP_OK;
}

esp_err_t usb_midi_set_event_handler(usb_midi_event_handler_t handler,
                                     void *context)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_event_handler = handler;
    s_event_handler_context = context;
    taskEXIT_CRITICAL(&s_status_lock);
    return ESP_OK;
}

void usb_midi_get_status(usb_midi_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_status_lock);
    *out_status = s_status;
    taskEXIT_CRITICAL(&s_status_lock);
}
