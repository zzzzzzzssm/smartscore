#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool connected;
    bool implemented;
} audio_s3_adapter_status_t;

/** Initialize the reserved S3 audio input adapter. */
esp_err_t audio_s3_adapter_init(void);

/** Start a fresh recognition session on the audio S3. */
esp_err_t audio_s3_adapter_start_session(void);

/** Start the relaxed recognition profile used only by the note monitor. */
esp_err_t audio_s3_adapter_start_demo_session(void);

/** Stop recognition events while keeping the S3 link alive. */
esp_err_t audio_s3_adapter_stop_session(void);

/** Return connection/implementation state without fabricating audio data. */
void audio_s3_adapter_get_status(audio_s3_adapter_status_t *out_status);

#ifdef __cplusplus
}
#endif
