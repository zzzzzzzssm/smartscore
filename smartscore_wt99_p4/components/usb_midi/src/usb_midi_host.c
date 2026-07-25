#include "usb_midi_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#define USB_AUDIO_CLASS 0x01
#define USB_MIDI_STREAMING_SUBCLASS 0x03
#define MIDI_TRANSFER_PACKETS 8

typedef enum {
    ACTION_NONE = 0,
    ACTION_OPEN = (1U << 0),
    ACTION_CLOSE = (1U << 1),
    ACTION_RECOVER = (1U << 2),
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
    char product[USB_MIDI_PRODUCT_MAX_LENGTH];
    volatile uint32_t actions;
    bool claimed;
    bool active;
    bool announced;
    bool closing;
    bool transfer_in_flight;
} midi_host_t;

static const char *TAG = "USB_MIDI_HOST";
static midi_host_t s_midi;

static bool find_midi_in_endpoint(const usb_config_desc_t *config,
                                  uint8_t *interface_number,
                                  uint8_t *alternate_setting,
                                  uint8_t *endpoint_address,
                                  uint16_t *endpoint_mps)
{
    const uint8_t *raw = (const uint8_t *)config;
    size_t total = config->wTotalLength;
    bool in_midi_interface = false;

    for (size_t offset = 0; offset + 2U <= total;) {
        uint8_t length = raw[offset];
        uint8_t type = raw[offset + 1U];
        if (length < 2U || offset + length > total) {
            ESP_LOGW(TAG, "malformed USB descriptor at offset %u",
                     (unsigned)offset);
            return false;
        }
        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE &&
            length >= USB_INTF_DESC_SIZE) {
            const usb_intf_desc_t *interface =
                (const usb_intf_desc_t *)(raw + offset);
            in_midi_interface =
                interface->bInterfaceClass == USB_AUDIO_CLASS &&
                interface->bInterfaceSubClass ==
                    USB_MIDI_STREAMING_SUBCLASS;
            if (in_midi_interface) {
                *interface_number = interface->bInterfaceNumber;
                *alternate_setting = interface->bAlternateSetting;
            }
        } else if (in_midi_interface &&
                   type == USB_B_DESCRIPTOR_TYPE_ENDPOINT &&
                   length >= USB_EP_DESC_SIZE) {
            const usb_ep_desc_t *endpoint =
                (const usb_ep_desc_t *)(raw + offset);
            uint8_t transfer_type = endpoint->bmAttributes &
                                    USB_BM_ATTRIBUTES_XFERTYPE_MASK;
            if ((endpoint->bEndpointAddress &
                 USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) != 0U &&
                (transfer_type == USB_BM_ATTRIBUTES_XFER_BULK ||
                 transfer_type == USB_BM_ATTRIBUTES_XFER_INT)) {
                *endpoint_address = endpoint->bEndpointAddress;
                *endpoint_mps = endpoint->wMaxPacketSize &
                                USB_W_MAX_PACKET_SIZE_MPS_MASK;
                return *endpoint_mps > 0U;
            }
        }
        offset += length;
    }
    return false;
}

static void usb_string_to_ascii(const usb_str_desc_t *descriptor,
                                char *output,
                                size_t output_size)
{
    if (output == NULL || output_size == 0U) {
        return;
    }
    output[0] = '\0';
    if (descriptor == NULL || descriptor->bLength < 2U) {
        return;
    }
    size_t characters = (descriptor->bLength - 2U) / 2U;
    size_t count = characters < output_size - 1U
                       ? characters
                       : output_size - 1U;
    for (size_t index = 0; index < count; ++index) {
        uint16_t codepoint = descriptor->wData[index];
        output[index] = codepoint >= 0x20U && codepoint <= 0x7eU &&
                                codepoint != '"' && codepoint != '\\'
                            ? (char)codepoint
                            : '?';
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
    host->announced = false;
    host->closing = false;
    host->transfer_in_flight = false;
}

static void close_device(midi_host_t *host)
{
    if (host->device == NULL || host->transfer_in_flight) {
        return;
    }
    if (host->announced) {
        usb_midi_publish_connection(false, 0, 0, NULL);
        ESP_LOGI(TAG, "USB MIDI device disconnected");
        host->announced = false;
    }
    if (host->claimed) {
        esp_err_t err = usb_host_interface_release(
            host->client, host->device, host->interface_number);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "interface release failed: %s",
                     esp_err_to_name(err));
        }
        host->claimed = false;
    }
    if (host->transfer != NULL) {
        esp_err_t err = usb_host_transfer_free(host->transfer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "transfer free failed: %s", esp_err_to_name(err));
        }
        host->transfer = NULL;
    }
    esp_err_t err = usb_host_device_close(host->client, host->device);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "device close failed: %s", esp_err_to_name(err));
    }
    reset_device_fields(host);
}

static void begin_close_device(midi_host_t *host)
{
    if (host->device == NULL) {
        return;
    }
    host->active = false;
    host->closing = true;
    if (host->announced) {
        usb_midi_publish_connection(false, 0, 0, NULL);
        host->announced = false;
    }
    if (host->endpoint_address != 0U) {
        esp_err_t err = usb_host_endpoint_halt(host->device,
                                               host->endpoint_address);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND &&
            err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "endpoint halt failed: %s", esp_err_to_name(err));
        }
        err = usb_host_endpoint_flush(host->device, host->endpoint_address);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND &&
            err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "endpoint flush failed: %s", esp_err_to_name(err));
        }
    }
    close_device(host);
}

static void transfer_callback(usb_transfer_t *transfer)
{
    midi_host_t *host = (midi_host_t *)transfer->context;
    host->transfer_in_flight = false;
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        if (transfer->actual_num_bytes > 0) {
            usb_midi_parse_transfer(transfer->data_buffer,
                                    (size_t)transfer->actual_num_bytes);
        }
    } else if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        host->active = false;
        host->closing = true;
        host->actions |= ACTION_CLOSE;
        return;
    } else if (transfer->status != USB_TRANSFER_STATUS_CANCELED) {
        host->actions |= ACTION_RECOVER;
        return;
    }

    if (host->active && !host->closing) {
        transfer->num_bytes = (int)host->transfer_size;
        esp_err_t err = usb_host_transfer_submit(transfer);
        if (err == ESP_OK) {
            host->transfer_in_flight = true;
        } else {
            usb_midi_publish_error(err, "usb_midi_transfer_resubmit_failed");
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
    if (host->device == NULL || !host->active || host->closing ||
        host->transfer_in_flight) {
        return;
    }
    esp_err_t err = usb_host_endpoint_clear(host->device,
                                            host->endpoint_address);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        usb_midi_publish_error(err, "usb_midi_endpoint_recovery_failed");
        host->active = false;
        host->closing = true;
        host->actions |= ACTION_CLOSE;
        return;
    }
    host->transfer->num_bytes = (int)host->transfer_size;
    err = usb_host_transfer_submit(host->transfer);
    if (err == ESP_OK) {
        host->transfer_in_flight = true;
    } else {
        usb_midi_publish_error(err, "usb_midi_transfer_resume_failed");
        host->active = false;
        host->closing = true;
        host->actions |= ACTION_CLOSE;
    }
}

static void open_device(midi_host_t *host, uint8_t address)
{
    if (host->device != NULL) {
        ESP_LOGW(TAG, "ignoring USB address %u; MIDI device already active",
                 address);
        return;
    }
    esp_err_t err = usb_host_device_open(host->client, address, &host->device);
    if (err != ESP_OK) {
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
    if (err == ESP_OK) {
        err = usb_host_device_info(host->device, &device_info);
    }
    if (err != ESP_OK || device_descriptor == NULL ||
        config_descriptor == NULL) {
        host->closing = true;
        close_device(host);
        return;
    }

    host->vid = device_descriptor->idVendor;
    host->pid = device_descriptor->idProduct;
    usb_string_to_ascii(device_info.str_desc_product, host->product,
                        sizeof(host->product));
    if (!find_midi_in_endpoint(config_descriptor, &host->interface_number,
                               &host->alternate_setting,
                               &host->endpoint_address,
                               &host->endpoint_mps)) {
        ESP_LOGI(TAG, "USB %04x:%04x is not a MIDI streaming device",
                 host->vid, host->pid);
        host->closing = true;
        close_device(host);
        return;
    }

    err = usb_host_interface_claim(host->client, host->device,
                                   host->interface_number,
                                   host->alternate_setting);
    if (err != ESP_OK) {
        host->closing = true;
        close_device(host);
        return;
    }
    host->claimed = true;
    host->transfer_size = (size_t)host->endpoint_mps *
                          MIDI_TRANSFER_PACKETS;
    err = usb_host_transfer_alloc(host->transfer_size, 0, &host->transfer);
    if (err != ESP_OK) {
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
        host->active = false;
        host->closing = true;
        close_device(host);
        return;
    }
    host->transfer_in_flight = true;
    host->announced = true;
    usb_midi_publish_connection(true, host->vid, host->pid, host->product);
    ESP_LOGI(TAG,
             "connected VID=%04x PID=%04x product=\"%s\" interface=%u endpoint=0x%02x",
             host->vid, host->pid,
             host->product[0] != '\0' ? host->product : "unknown",
             host->interface_number, host->endpoint_address);
}

static void client_event_callback(const usb_host_client_event_msg_t *event,
                                  void *argument)
{
    midi_host_t *host = (midi_host_t *)argument;
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        if (host->device == NULL || host->closing) {
            host->pending_address = event->new_dev.address;
            host->actions |= ACTION_OPEN;
        }
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE &&
               host->device == event->dev_gone.dev_hdl) {
        host->active = false;
        host->closing = true;
        host->actions |= ACTION_CLOSE;
    }
}

static void midi_client_task(void *argument)
{
    (void)argument;
    usb_host_client_config_t config = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = client_event_callback,
            .callback_arg = &s_midi,
        },
    };
    esp_err_t err = usb_host_client_register(&config, &s_midi.client);
    if (err != ESP_OK) {
        usb_midi_publish_error(err, "usb_midi_client_register_failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "MIDI class client registered");
    while (true) {
        err = usb_host_client_handle_events(s_midi.client,
                                            pdMS_TO_TICKS(50));
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            usb_midi_publish_error(err, "usb_midi_client_event_failed");
        }
        uint32_t actions = s_midi.actions;
        s_midi.actions &= ~actions;
        if ((actions & ACTION_CLOSE) != 0U) {
            begin_close_device(&s_midi);
        }
        if ((actions & ACTION_RECOVER) != 0U) {
            recover_endpoint(&s_midi);
        }
        if ((actions & ACTION_OPEN) != 0U && s_midi.device == NULL) {
            open_device(&s_midi, s_midi.pending_address);
        } else if ((actions & ACTION_OPEN) != 0U) {
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
        xTaskNotify(starter, (uint32_t)err, eSetValueWithOverwrite);
        vTaskDelete(NULL);
        return;
    }
    xTaskNotify(starter, (uint32_t)ESP_OK, eSetValueWithOverwrite);
    ESP_LOGI(TAG, "USB Host installed on ESP32-P4 default peripheral");
    while (true) {
        uint32_t flags = 0;
        err = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (err != ESP_OK) {
            usb_midi_publish_error(err, "usb_host_daemon_event_failed");
        }
    }
}

esp_err_t usb_midi_host_start(void)
{
    memset(&s_midi, 0, sizeof(s_midi));
    if (xTaskCreate(usb_daemon_task, "usb_daemon", 4096,
                    xTaskGetCurrentTaskHandle(), 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    uint32_t install_result = (uint32_t)ESP_FAIL;
    if (xTaskNotifyWait(0, UINT32_MAX, &install_result,
                        pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if ((esp_err_t)install_result != ESP_OK) {
        return (esp_err_t)install_result;
    }
    if (xTaskCreate(midi_client_task, "usb_midi_client", 6144,
                    NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
