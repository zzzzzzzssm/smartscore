idf_component_register(
    SRCS "src/deepseek_advice.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_client
        esp-tls
        json
)

set(DEEPSEEK_BUILD_API_KEY "$ENV{DEEPSEEK_API_KEY}")
if(NOT DEEPSEEK_BUILD_API_KEY STREQUAL "")
    string(REPLACE "\\" "\\\\" DEEPSEEK_BUILD_API_KEY "${DEEPSEEK_BUILD_API_KEY}")
    string(REPLACE "\"" "\\\"" DEEPSEEK_BUILD_API_KEY "${DEEPSEEK_BUILD_API_KEY}")
    target_compile_definitions(
        ${COMPONENT_LIB}
        PRIVATE "DEEPSEEK_API_KEY=\"${DEEPSEEK_BUILD_API_KEY}\""
    )
endif()

