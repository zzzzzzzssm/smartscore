idf_component_register(
    SRCS "audio_capture.c"
    INCLUDE_DIRS "."
    REQUIRES
        m5stack_tab5
        esp_codec_dev
        driver
)
