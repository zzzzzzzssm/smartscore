idf_component_register(
    SRCS
        "src/voice_assistant_v2.c"
        "src/voice_v2_protocol.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_websocket_client
        esp-tls
        esp_timer
        freertos
        json
        mbedtls
        heap
        speaker_service
)

# Qwen Audio Realtime uses the same Alibaba Cloud Model Studio credential as
# the existing DashScope OMR component.  Inject it only into this private
# build target; never write the key to sdkconfig, source, logs, or Git.
set(QWEN_AUDIO_BUILD_API_KEY "$ENV{DASHSCOPE_API_KEY}")
if(NOT QWEN_AUDIO_BUILD_API_KEY STREQUAL "")
    string(REPLACE "\\" "\\\\" QWEN_AUDIO_BUILD_API_KEY
           "${QWEN_AUDIO_BUILD_API_KEY}")
    string(REPLACE "\"" "\\\"" QWEN_AUDIO_BUILD_API_KEY
           "${QWEN_AUDIO_BUILD_API_KEY}")
    target_compile_definitions(
        ${COMPONENT_LIB}
        PRIVATE "QWEN_AUDIO_API_KEY=\"${QWEN_AUDIO_BUILD_API_KEY}\""
    )
endif()
unset(QWEN_AUDIO_BUILD_API_KEY)
