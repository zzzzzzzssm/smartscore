cmake_minimum_required(VERSION 3.20)
project(practice_advice_state_host_tests C)

add_executable(
    practice_advice_state_host_tests
    test_practice_advice_state.c
    ../../components/practice_advice_service/src/practice_advice_state.c
)

target_include_directories(
    practice_advice_state_host_tests
    PRIVATE ../../components/practice_advice_service/src
)

if(MSVC)
    target_compile_options(practice_advice_state_host_tests PRIVATE /utf-8 /W4)
else()
    target_compile_options(practice_advice_state_host_tests PRIVATE -Wall -Wextra)
endif()
