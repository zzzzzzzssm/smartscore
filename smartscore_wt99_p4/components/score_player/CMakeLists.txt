idf_component_register(
    SRCS
        "src/piano_sample_bank.c"
        "src/score_player.c"
    INCLUDE_DIRS "include"
    REQUIRES
        freertos
        heap
        json
        speaker_service
)
