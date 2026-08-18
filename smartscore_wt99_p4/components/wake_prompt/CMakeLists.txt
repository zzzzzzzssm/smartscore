idf_component_register(
    SRCS "src/wake_prompt.c"
    INCLUDE_DIRS "include"
    REQUIRES
        board_wt99
        speaker_service
        freertos
)
