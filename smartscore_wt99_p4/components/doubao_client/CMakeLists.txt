idf_component_register(
    SRCS "src/doubao_client.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_client
        esp-tls
        json
        mbedtls
        heap
)

set(DOUBAO_BUILD_API_KEY "$ENV{VEI_API_KEY}")
if(NOT DOUBAO_BUILD_API_KEY STREQUAL "")
    string(REPLACE "\\" "\\\\" DOUBAO_BUILD_API_KEY "${DOUBAO_BUILD_API_KEY}")
    string(REPLACE "\"" "\\\"" DOUBAO_BUILD_API_KEY "${DOUBAO_BUILD_API_KEY}")
    target_compile_definitions(
        ${COMPONENT_LIB}
        PRIVATE "VEI_API_KEY=\"${DOUBAO_BUILD_API_KEY}\""
    )
endif()

