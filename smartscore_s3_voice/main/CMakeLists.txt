idf_component_register(
    SRCS
        "app_main.c"
        "audio_inmp441.c"
        "qwen_protocol.c"
        "qwen_realtime.c"
        "voice_link_protocol.c"
        "voice_recognition.c"
        "voice_uart_link.c"
    INCLUDE_DIRS "."
    REQUIRES esp-sr driver esp_psram spi_flash esp_websocket_client
             esp-tls esp_wifi esp_event esp_netif nvs_flash json mbedtls
)

set(QWEN_S3_BUILD_API_KEY "$ENV{DASHSCOPE_API_KEY}")
if(NOT QWEN_S3_BUILD_API_KEY STREQUAL "")
    string(REPLACE "\\" "\\\\" QWEN_S3_BUILD_API_KEY
           "${QWEN_S3_BUILD_API_KEY}")
    string(REPLACE "\"" "\\\"" QWEN_S3_BUILD_API_KEY
           "${QWEN_S3_BUILD_API_KEY}")
    target_compile_definitions(
        ${COMPONENT_LIB}
        PRIVATE "QWEN_S3_API_KEY=\"${QWEN_S3_BUILD_API_KEY}\""
    )
endif()
unset(QWEN_S3_BUILD_API_KEY)
