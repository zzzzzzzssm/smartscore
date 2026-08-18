#include "audio_s3_adapter.h"

#include "s3_bus.h"

static uint32_t s_next_sid;

static uint32_t next_sid(void)
{
    ++s_next_sid;
    if (s_next_sid == 0) {
        ++s_next_sid;
    }
    return s_next_sid;
}

esp_err_t audio_s3_adapter_init(void)
{
    return ESP_OK;
}

esp_err_t audio_s3_adapter_start_session(void)
{
    return s3_bus_start_stream(next_sid());
}

esp_err_t audio_s3_adapter_start_demo_session(void)
{
    return s3_bus_start_stream_with_profile(
        next_sid(), S3_MUSIC_STREAM_PROFILE_DEMO);
}

esp_err_t audio_s3_adapter_stop_session(void)
{
    return s3_bus_stop_stream();
}

void audio_s3_adapter_get_status(audio_s3_adapter_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }
    s3_bus_status_t bus;
    s3_bus_get_status(&bus);
    *out_status = (audio_s3_adapter_status_t) {
        .connected = bus.online,
        .implemented = true,
    };
}
