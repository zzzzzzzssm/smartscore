#include "performance_types.h"

#include <string.h>

const char *input_source_name(input_source_t source)
{
    switch (source) {
        case INPUT_SOURCE_USB_MIDI:
            return "usb_midi";
        case INPUT_SOURCE_AUDIO_S3:
            return "audio_s3";
        case INPUT_SOURCE_NONE:
        default:
            return "none";
    }
}

bool input_source_from_name(const char *name, input_source_t *out_source)
{
    if (name == NULL || out_source == NULL) {
        return false;
    }
    if (strcmp(name, "usb_midi") == 0) {
        *out_source = INPUT_SOURCE_USB_MIDI;
        return true;
    }
    if (strcmp(name, "audio_s3") == 0) {
        *out_source = INPUT_SOURCE_AUDIO_S3;
        return true;
    }
    return false;
}
