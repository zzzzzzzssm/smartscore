idf_component_register(
    SRCS
        "app_main.c"
        "audio_inmp441.c"
        "voice_ai_stream.c"
        "voice_ima_adpcm.c"
        "voice_link_protocol.c"
        "voice_recognition.c"
        "voice_uart_link.c"
    INCLUDE_DIRS "."
    REQUIRES esp-sr driver esp_psram spi_flash
)
