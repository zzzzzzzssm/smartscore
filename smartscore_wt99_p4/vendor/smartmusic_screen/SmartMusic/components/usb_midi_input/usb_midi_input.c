#include "usb_midi_input.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#define USB_AUDIO_CLASS             0x01
#define USB_MIDI_STREAMING_SUBCLASS 0x03
#define MIDI_TRANSFER_PACKETS       8
#define MIDI_EVENT_QUEUE_LENGTH     64

typedef enum {
    ACTION_NONE = 0,
    ACTION_OPEN = (1 << 0),
    ACTION_CLOSE = (1 << 1),
    ACTION_RECOVER = (1 << 2),
} midi_action_t;

typedef struct {
    usb_host_client_handle_t client;
    usb_device_handle_t device;
    usb_transfer_t *transfer;
    uint8_t pending_address;
    uint8_t interface_number;
    uint8_t alternate_setting;
    uint8_t endpoint_address;
    uint16_t endpoint_mps;
    size_t transfer_size;
    uint16_t vid;
    uint16_t pid;
    char product[64];
    volatile uint32_t actions;
    bool claimed;
    bool active;
    bool closing;
    bool transfer_in_flight;
    bool device_announced;
} midi_host_t;

static const char *TAG = "usb_midi_input";
static midi_host_t s_midi;
static QueueHandle_t s_event_queue;
static bool s_started;

static void publish_event(const usb_midi_input_event_t *event)
{
    if (!s_event_queue || !event) return;
    if (xQueueSend(s_event_queue, event, 0) != pdPASS) {
        ESP_LOGW(TAG, "MIDI event queue full; dropping event type %d", event->type);
    }
}

static const char *note_name(uint8_t note, char *buffer, size_t size)
{
    static const char *const names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    snprintf(buffer, size, "%s%d", names[note % 12], (int)(note / 12) - 1);
    return buffer;
}

static void process_usb_midi_packets(const uint8_t *data, size_t length)
{
    if ((length % 4) != 0) {
        ESP_LOGW(TAG, "USB-MIDI transfer has %u trailing byte(s)",
                 (unsigned)(length % 4));
    }

    for (size_t offset = 0; offset + 3 < length; offset += 4) {
        const uint8_t cable = data[offset] >> 4;
        const uint8_t status = data[offset + 1];
        const uint8_t message = status & 0xf0;
        const uint8_t channel = status & 0x0f;
        const uint8_t note = data[offset + 2] & 0x7f;
        const uint8_t velocity = data[offset + 3] & 0x7f;
        usb_midi_input_event_type_t type;

        if (message == 0x90 && velocity != 0) {
            type = USB_MIDI_INPUT_NOTE_ON;
        } else if (message == 0x80 || (message == 0x90 && velocity == 0)) {
            type = USB_MIDI_INPUT_NOTE_OFF;
        } else {
            continue;
        }

        usb_midi_input_event_t event = {
            .type = type,
            .timestamp_us = (uint64_t)esp_timer_get_time(),
            .cable = cable,
            .channel = channel,
            .note = note,
            .velocity = velocity,
        };
        publish_event(&event);

        char name[8];
        ESP_LOGI(TAG, "MIDI %-8s ch=%u cable=%u note=%u(%s) velocity=%u",
                 type == USB_MIDI_INPUT_NOTE_ON ? "Note On" : "Note Off",
                 channel + 1, cable, note,
                 note_name(note, name, sizeof(name)), velocity);
    }
}

static void transfer_callback(usb_transfer_t *transfer)
{
    midi_host_t *host = (midi_host_t *)transfer->context;
    host->transfer_in_flight = false;

    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        if (transfer->actual_num_bytes > 0) {
            process_usb_midi_packets(transfer->data_buffer,
                                     (size_t)transfer->actual_num_bytes);
        }
    } else if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        host->active = false;
        host->closing = true;
        host->actions |= ACTION_CLOSE;
        return;
    } else if (transfer->status != USB_TRANSFER_STATUS_CANCELED) {
        ESP_LOGW(TAG, "MIDI IN transfer ended with status %d", transfer->status);
        host->actions |= ACTION_RECOVER;
        return;
    }

    if (host->active && !host->closing) {
        transfer->num_bytes = (int)host->transfer_size;
        esp_err_t err = usb_host_transfer_submit(transfer);
        if (err == ESP_OK) {
            host->transfer_in_flight = true;
        } else {
            ESP_LOGE(TAG, "Unable to resubmit MIDI IN transfer: %s",
                     esp_err_to_name(err));
            host->active = false;
            host->closing = true;
            host->actions |= ACTION_CLOSE;
        }
    } else if (host->closing) {
        host->actions |= ACTION_CLOSE;
    }
}

static void recover_endpoint(midi_host_t *host)
{
    if (!host->device || !host->active || host->closing ||
        host->transfer_in_flight) return;

    esp_err_t err = usb_host_endpoint_clear(host->device,
                                            host->endpoint_address);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Unable to clear MIDI IN endpoint: %s",
                 esp_err_to_name(err));
        host->active = false;
        host->closing = true;
        host->actions |= ACTION_CLOSE;
        return;
    }

    host->transfer->num_bytes = (int)host->transfer_size;
    err = usb_host_transfer_submit(host->transfer);
    if (err == ESP_OK) {
        host->transfer_in_flight = true;
        ESP_LOGI(TAG, "MIDI IN endpoint recovered");
    } else {
        ESP_LOGE(TAG, "Unable to resume MIDI IN transfer: %s",
                 esp_err_to_name(err));
        host->active = false;
        host->closing = true;
        host->actions |= ACTION_CLOSE;
    }
}

static bool find_midi_in_endpoint(const usb_config_desc_t *config,
                                  uint8_t *interface_number,
                                  uint8_t *alternate_setting,
                                  uint8_t *endpoint_address,
                                  uint16_t *endpoint_mps)
{
    const uint8_t *raw = (const uint8_t *)config;
    const size_t total = config->wTotalLength;
    bool in_midi_interface = false;

    for (size_t offset = 0; offset + 2 <= total;) {
        const uint8_t length = raw[offset];
        const uint8_t type = raw[offset + 1];
        if (length < 2 || offset + length > total) {
            ESP_LOGW(TAG, "Malformed USB descriptor at offset %u",
                     (unsigned)offset);
            return false;
        }

        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE &&
            length >= USB_INTF_DESC_SIZE) {
            const usb_intf_desc_t *interface =
                (const usb_intf_desc_t *)(raw + offset);
            in_midi_interface =
                interface->bInterfaceClass == USB_AUDIO_CLASS &&
                interface->bInterfaceSubClass == USB_MIDI_STREAMING_SUBCLASS;
            if (in_midi_interface) {
                *interface_number = interface->bInterfaceNumber;
                *alternate_setting = interface->bAlternateSetting;
            }
        } else if (in_midi_interface &&
                   type == USB_B_DESCRIPTOR_TYPE_ENDPOINT &&
                   length >= USB_EP_DESC_SIZE) {
            const usb_ep_desc_t *endpoint =
                (const usb_ep_desc_t *)(raw + offset);
            const uint8_t transfer_type =
                endpoint->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;
            if ((endpoint->bEndpointAddress &
                 USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) &&
                (transfer_type == USB_BM_ATTRIBUTES_XFER_BULK ||
                 transfer_type == USB_BM_ATTRIBUTES_XFER_INT)) {
                *endpoint_address = endpoint->bEndpointAddress;
                *endpoint_mps = endpoint->wMaxPacketSize &
                                USB_W_MAX_PACKET_SIZE_MPS_MASK;
                return *endpoint_mps > 0;
            }
        }
        offset += length;
    }
    return false;
}

static void usb_string_to_utf8(const usb_str_desc_t *descriptor, char *output,
                               size_t output_size)
{
    if (!output_size) return;
    output[0] = '\0';
    if (!descriptor || descriptor->bLength < 2) return;

    const size_t characters = (descriptor->bLength - 2) / 2;
    const size_t count = characters < output_size - 1 ?
                         characters : output_size - 1;
    for (size_t i = 0; i < count; ++i) {
        const uint16_t codepoint = descriptor->wData[i];
        output[i] = codepoint >= 0x20 && codepoint <= 0x7e &&
                    codepoint != '"' && codepoint != '\\' ?
                    (char)codepoint : '?';
    }
    output[count] = '\0';
}

static void reset_device_fields(midi_host_t *host)
{
    host->device = NULL;
    host->transfer = NULL;
    host->interface_number = 0;
    host->alternate_setting = 0;
    host->endpoint_address = 0;
    host->endpoint_mps = 0;
    host->transfer_size = 0;
    host->vid = 0;
    host->pid = 0;
    host->product[0] = '\0';
    host->claimed = false;
    host->active = false;
    host->closing = false;
    host->transfer_in_flight = false;
    host->device_announced = false;
}

static void announce_disconnected(midi_host_t *host)
{
    if (!host->device_announced) return;
    usb_midi_input_event_t event = {
        .type = USB_MIDI_INPUT_DEVICE_DISCONNECTED,
        .timestamp_us = (uint64_t)esp_timer_get_time(),
        .vid = host->vid,
        .pid = host->pid,
    };
    strlcpy(event.product, host->product, sizeof(event.product));
    publish_event(&event);
    host->device_announced = false;
}

static void close_device(midi_host_t *host)
{
    if (!host->device || host->transfer_in_flight) return;

    announce_disconnected(host);
    if (host->claimed) {
        esp_err_t err = usb_host_interface_release(
            host->client, host->device, host->interface_number);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "Interface release: %s", esp_err_to_name(err));
        }
        host->claimed = false;
    }
    esp_err_t err = usb_host_device_close(host->client, host->device);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Device close: %s", esp_err_to_name(err));
    }
    if (host->transfer) {
        err = usb_host_transfer_free(host->transfer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Transfer free: %s", esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "USB MIDI device disconnected");
    reset_device_fields(host);
}

static void begin_close_device(midi_host_t *host)
{
    if (!host->device) return;
    host->active = false;
    host->closing = true;
    announce_disconnected(host);

    if (host->endpoint_address) {
        esp_err_t err = usb_host_endpoint_halt(host->device,
                                               host->endpoint_address);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND &&
            err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Endpoint halt: %s", esp_err_to_name(err));
        }
        err = usb_host_endpoint_flush(host->device, host->endpoint_address);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND &&
            err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Endpoint flush: %s", esp_err_to_name(err));
        }
    }
    close_device(host);
}

static void open_device(midi_host_t *host, uint8_t address)
{
    if (host->device) {
        ESP_LOGW(TAG, "Ignoring USB address %u; one device is already active",
                 address);
        return;
    }

    esp_err_t err = usb_host_device_open(host->client, address, &host->device);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to open USB address %u: %s", address,
                 esp_err_to_name(err));
        reset_device_fields(host);
        return;
    }

    const usb_device_desc_t *device_descriptor = NULL;
    const usb_config_desc_t *config_descriptor = NULL;
    usb_device_info_t device_info = {0};
    err = usb_host_get_device_descriptor(host->device, &device_descriptor);
    if (err == ESP_OK) {
        err = usb_host_get_active_config_descriptor(host->device,
                                                    &config_descriptor);
    }
    if (err == ESP_OK) err = usb_host_device_info(host->device, &device_info);
    if (err != ESP_OK || !device_descriptor || !config_descriptor) {
        ESP_LOGW(TAG, "Unable to read USB descriptors: %s",
                 esp_err_to_name(err));
        host->closing = true;
        close_device(host);
        return;
    }

    host->vid = device_descriptor->idVendor;
    host->pid = device_descriptor->idProduct;
    usb_string_to_utf8(device_info.str_desc_product, host->product,
                       sizeof(host->product));

    if (!find_midi_in_endpoint(config_descriptor, &host->interface_number,
                               &host->alternate_setting,
                               &host->endpoint_address,
                               &host->endpoint_mps)) {
        ESP_LOGI(TAG, "USB device %04x:%04x is not USB-MIDI 1.0",
                 host->vid, host->pid);
        host->closing = true;
        close_device(host);
        return;
    }

    err = usb_host_interface_claim(host->client, host->device,
                                   host->interface_number,
                                   host->alternate_setting);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to claim MIDI interface %u: %s",
                 host->interface_number, esp_err_to_name(err));
        host->closing = true;
        close_device(host);
        return;
    }
    host->claimed = true;
    host->transfer_size = (size_t)host->endpoint_mps * MIDI_TRANSFER_PACKETS;

    err = usb_host_transfer_alloc(host->transfer_size, 0, &host->transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Unable to allocate MIDI transfer: %s",
                 esp_err_to_name(err));
        host->closing = true;
        close_device(host);
        return;
    }
    host->transfer->device_handle = host->device;
    host->transfer->bEndpointAddress = host->endpoint_address;
    host->transfer->callback = transfer_callback;
    host->transfer->context = host;
    host->transfer->num_bytes = (int)host->transfer_size;

    host->active = true;
    host->closing = false;
    err = usb_host_transfer_submit(host->transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Unable to submit MIDI IN transfer: %s",
                 esp_err_to_name(err));
        host->active = false;
        host->closing = true;
        close_device(host);
        return;
    }
    host->transfer_in_flight = true;
    host->device_announced = true;

    usb_midi_input_event_t event = {
        .type = USB_MIDI_INPUT_DEVICE_CONNECTED,
        .timestamp_us = (uint64_t)esp_timer_get_time(),
        .vid = host->vid,
        .pid = host->pid,
    };
    strlcpy(event.product, host->product, sizeof(event.product));
    publish_event(&event);

    ESP_LOGI(TAG, "USB MIDI connected: VID=%04x PID=%04x product=\"%s\"",
             host->vid, host->pid,
             host->product[0] ? host->product : "unknown");
    ESP_LOGI(TAG, "interface=%u alt=%u IN endpoint=0x%02x MPS=%u",
             host->interface_number, host->alternate_setting,
             host->endpoint_address, host->endpoint_mps);
}

static void client_event_callback(const usb_host_client_event_msg_t *event,
                                  void *arg)
{
    midi_host_t *host = (midi_host_t *)arg;
    switch (event->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        if (!host->device || host->closing) {
            host->pending_address = event->new_dev.address;
            host->actions |= ACTION_OPEN;
        }
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        if (host->device == event->dev_gone.dev_hdl) {
            host->active = false;
            host->closing = true;
            host->actions |= ACTION_CLOSE;
        }
        break;
    default:
        break;
    }
}

static void midi_client_task(void *argument)
{
    (void)argument;
    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = client_event_callback,
            .callback_arg = &s_midi,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &s_midi.client));
    ESP_LOGI(TAG, "USB-MIDI client registered; waiting for keyboard");

    for (;;) {
        usb_host_client_handle_events(s_midi.client, pdMS_TO_TICKS(50));
        const uint32_t actions = s_midi.actions;
        s_midi.actions &= ~actions;
        if (actions & ACTION_CLOSE) begin_close_device(&s_midi);
        if (actions & ACTION_RECOVER) recover_endpoint(&s_midi);
        if ((actions & ACTION_OPEN) && !s_midi.device) {
            open_device(&s_midi, s_midi.pending_address);
        } else if (actions & ACTION_OPEN) {
            s_midi.actions |= ACTION_OPEN;
        }
        if (s_midi.closing && !s_midi.transfer_in_flight) {
            close_device(&s_midi);
        }
    }
}

static void usb_daemon_task(void *argument)
{
    TaskHandle_t starter = (TaskHandle_t)argument;
    usb_host_config_t config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB Host install failed: %s", esp_err_to_name(err));
        xTaskNotify(starter, (uint32_t)err, eSetValueWithOverwrite);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "USB Host installed on ESP32-P4 High-Speed peripheral");
    xTaskNotify(starter, (uint32_t)ESP_OK, eSetValueWithOverwrite);
    for (;;) {
        uint32_t flags = 0;
        err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "USB Host daemon event error: %s",
                     esp_err_to_name(err));
        }
    }
}

esp_err_t usb_midi_input_start(void)
{
    if (s_started) return ESP_OK;
    s_event_queue = xQueueCreate(MIDI_EVENT_QUEUE_LENGTH,
                                 sizeof(usb_midi_input_event_t));
    if (!s_event_queue) return ESP_ERR_NO_MEM;

    memset(&s_midi, 0, sizeof(s_midi));
    TaskHandle_t daemon = NULL;
    if (xTaskCreate(usb_daemon_task, "usb_daemon", 4096,
                    xTaskGetCurrentTaskHandle(), 5, &daemon) != pdPASS) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    uint32_t install_result = ESP_FAIL;
    if (xTaskNotifyWait(0, UINT32_MAX, &install_result,
                        pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out waiting for USB Host installation");
        return ESP_ERR_TIMEOUT;
    }
    if ((esp_err_t)install_result != ESP_OK) {
        return (esp_err_t)install_result;
    }
    if (xTaskCreate(midi_client_task, "usb_midi", 6144, NULL, 6, NULL) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

QueueHandle_t usb_midi_input_event_queue(void)
{
    return s_event_queue;
}
