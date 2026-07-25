#include "ble_provisioning_internal.h"

#include <string.h>

#include "ble_provisioning_protocol.h"
#include "cJSON.h"

static uint8_t s_buffer[BLE_PROVISIONING_MAX_MESSAGE_BYTES + 1];
static size_t s_length;
static bool s_discarding;

void ble_message_codec_reset(void)
{
    memset(s_buffer, 0, sizeof(s_buffer));
    s_length = 0;
    s_discarding = false;
}

static void clear_password(ble_decoded_message_t *message)
{
    volatile char *bytes = message->password;
    for (size_t index = 0; index < sizeof(message->password); ++index) {
        bytes[index] = 0;
    }
}

static void parse_complete_message(ble_message_callback_t callback, void *context)
{
    ble_decoded_message_t message = {0};
    message.request_id = 0;
    s_buffer[s_length] = '\0';

    cJSON *root = cJSON_ParseWithLength((const char *)s_buffer, s_length);
    if (root == NULL || !cJSON_IsObject(root)) {
        strlcpy(message.error, "INVALID_JSON", sizeof(message.error));
        callback(&message, context);
        cJSON_Delete(root);
        return;
    }

    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsNumber(id) || !cJSON_IsString(command) ||
        command->valuestring == NULL) {
        strlcpy(message.error, "INVALID_MESSAGE", sizeof(message.error));
        callback(&message, context);
        cJSON_Delete(root);
        return;
    }
    message.request_id = id->valueint;

    if (strcmp(command->valuestring, "get_status") == 0) {
        message.command = BLE_COMMAND_GET_STATUS;
    } else if (strcmp(command->valuestring, "scan_wifi") == 0) {
        message.command = BLE_COMMAND_SCAN_WIFI;
    } else if (strcmp(command->valuestring, "reset_wifi") == 0) {
        message.command = BLE_COMMAND_RESET_WIFI;
    } else if (strcmp(command->valuestring, "get_device_info") == 0) {
        message.command = BLE_COMMAND_GET_DEVICE_INFO;
    } else if (strcmp(command->valuestring, "set_wifi") == 0) {
        cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
        if (!cJSON_IsString(ssid) || !cJSON_IsString(password) ||
            ssid->valuestring == NULL || password->valuestring == NULL) {
            strlcpy(message.error, "INVALID_CREDENTIALS", sizeof(message.error));
        } else {
            size_t ssid_length = strlen(ssid->valuestring);
            size_t password_length = strlen(password->valuestring);
            if (ssid_length == 0 || ssid_length > 32 || password_length > 64) {
                strlcpy(message.error, "INVALID_CREDENTIALS", sizeof(message.error));
            } else {
                message.command = BLE_COMMAND_SET_WIFI;
                memcpy(message.ssid, ssid->valuestring, ssid_length + 1);
                memcpy(message.password, password->valuestring, password_length + 1);
            }
        }
    } else {
        strlcpy(message.error, "UNKNOWN_COMMAND", sizeof(message.error));
    }

    callback(&message, context);
    clear_password(&message);
    cJSON_Delete(root);
}

esp_err_t ble_message_codec_feed(const uint8_t *data,
                                 size_t length,
                                 ble_message_callback_t callback,
                                 void *context)
{
    if (data == NULL || callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = ESP_OK;
    for (size_t index = 0; index < length; ++index) {
        uint8_t byte = data[index];
        if (s_discarding) {
            if (byte == '\n') {
                s_discarding = false;
                s_length = 0;
            }
            continue;
        }
        if (byte == '\n') {
            if (s_length > 0) {
                parse_complete_message(callback, context);
            }
            memset(s_buffer, 0, s_length);
            s_length = 0;
            continue;
        }
        if (s_length >= BLE_PROVISIONING_MAX_MESSAGE_BYTES) {
            memset(s_buffer, 0, sizeof(s_buffer));
            s_length = 0;
            s_discarding = true;
            result = ESP_ERR_INVALID_SIZE;
            continue;
        }
        s_buffer[s_length++] = byte;
    }
    return result;
}
