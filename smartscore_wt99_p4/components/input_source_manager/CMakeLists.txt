idf_component_register(
    SRCS "src/input_source_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES
        audio_s3_adapter
        common
        nvs_flash
        usb_midi
)
