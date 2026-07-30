#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "performance_types.h"
#include "usb_midi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCORING_PROFILE_NAME_MAX_LENGTH 32

typedef enum {
    SCORING_SERVICE_UNINITIALIZED = 0,
    SCORING_SERVICE_IDLE,
    SCORING_SERVICE_RECORDING,
    SCORING_SERVICE_PAUSED,
    SCORING_SERVICE_SCORING,
    SCORING_SERVICE_READY,
    SCORING_SERVICE_ERROR,
} scoring_service_state_t;

typedef struct {
    scoring_service_state_t state;
    input_source_t input_source;
    char profile[SCORING_PROFILE_NAME_MAX_LENGTH];
    size_t target_count;
    size_t played_count;
    esp_err_t last_error;
    char error[64];
    char message[96];
} scoring_service_status_t;

typedef esp_err_t (*scoring_result_consumer_t)(const char *json,
                                                size_t length,
                                                void *context);

esp_err_t scoring_service_init(void);
esp_err_t scoring_service_start(const char *input_source,
                                const char *profile);
esp_err_t scoring_service_pause(void);
esp_err_t scoring_service_resume(void);
esp_err_t scoring_service_stop(void);
esp_err_t scoring_service_reset(void);
esp_err_t scoring_service_wait_for_result(uint32_t timeout_ms);
esp_err_t scoring_service_with_result(scoring_result_consumer_t consumer,
                                      void *context);
void scoring_service_handle_usb_event(const usb_midi_event_t *event);
void scoring_service_handle_audio_s3_event(bool note_on,
                                           uint32_t sid,
                                           uint32_t sender_ts_ms,
                                           uint8_t midi,
                                           uint8_t velocity,
                                           float confidence,
                                           float frequency_hz);
void scoring_service_get_status(scoring_service_status_t *out_status);
const char *scoring_service_state_name(scoring_service_state_t state);

#ifdef __cplusplus
}
#endif
