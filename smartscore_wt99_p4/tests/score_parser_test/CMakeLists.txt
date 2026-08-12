cmake_minimum_required(VERSION 3.20)
project(compact_score_host_tests C)

if(NOT IDF_JSON_DIR)
    message(FATAL_ERROR "IDF_JSON_DIR must point to ESP-IDF components/json/cJSON")
endif()

add_executable(
    compact_score_host_tests
    test_compact_score.c
    ../../components/compact_score/src/compact_score.c
    ../../components/dashscope_omr/src/dashscope_omr_response.c
    ${IDF_JSON_DIR}/cJSON.c
)

target_include_directories(
    compact_score_host_tests
    PRIVATE
        ../../components/compact_score/include
        ../../components/dashscope_omr/include
        ${IDF_JSON_DIR}
)

if(MSVC)
    target_compile_options(compact_score_host_tests PRIVATE /utf-8 /W4)
else()
    target_compile_options(compact_score_host_tests PRIVATE -Wall -Wextra)
    target_link_libraries(compact_score_host_tests PRIVATE m)
endif()

