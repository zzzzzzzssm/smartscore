idf_component_register(
    SRCS "audio_session.c"
    INCLUDE_DIRS "."
    REQUIRES
        smartscore_core
        audio_capture
        json
        esp_timer
)
