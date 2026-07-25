idf_component_register(
    SRCS "src/performance_recorder.c"
    INCLUDE_DIRS "include"
    REQUIRES
        common
        esp_timer
        heap
)
