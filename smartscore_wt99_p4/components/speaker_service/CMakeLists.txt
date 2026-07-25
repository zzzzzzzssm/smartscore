idf_component_register(
    SRCS
        "src/speaker_service.c"
        "src/wav_stream.c"
    INCLUDE_DIRS "include"
    REQUIRES
        board_wt99
        esp_timer
        freertos
)
