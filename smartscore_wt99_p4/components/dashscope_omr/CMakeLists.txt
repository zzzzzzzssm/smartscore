set(DASHSCOPE_OMR_API_KEY_VALUE "$ENV{DASHSCOPE_API_KEY}")

if(DASHSCOPE_OMR_API_KEY_VALUE STREQUAL "" OR
   DASHSCOPE_OMR_API_KEY_VALUE STREQUAL "__DASHSCOPE_API_KEY__")
    message(FATAL_ERROR
        "DASHSCOPE_API_KEY is not set. Set it in the current shell before "
        "running idf.py reconfigure/build; the value will not be logged.")
endif()

if(DASHSCOPE_OMR_API_KEY_VALUE MATCHES "[\r\n\"\\\\]")
    message(FATAL_ERROR
        "DASHSCOPE_API_KEY contains an unsupported character; the value "
        "will not be logged.")
endif()

set(DASHSCOPE_OMR_PRIVATE_INCLUDE_DIR
    "${CMAKE_BINARY_DIR}/private_include/dashscope_omr")
file(MAKE_DIRECTORY "${DASHSCOPE_OMR_PRIVATE_INCLUDE_DIR}")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/dashscope_omr_private_config.h.in"
    "${DASHSCOPE_OMR_PRIVATE_INCLUDE_DIR}/dashscope_omr_private_config.h"
    @ONLY
    NEWLINE_STYLE UNIX
)

idf_component_register(
    SRCS
        "src/dashscope_omr.c"
        "src/dashscope_omr_prompt.c"
        "src/dashscope_omr_response.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${DASHSCOPE_OMR_PRIVATE_INCLUDE_DIR}"
    REQUIRES
        compact_score
        score_capture
        esp_http_client
        esp-tls
        esp_timer
        freertos
        json
        mbedtls
)

unset(DASHSCOPE_OMR_API_KEY_VALUE)
