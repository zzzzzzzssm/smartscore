idf_component_register(
    SRCS "src/scoring_service.c"
    INCLUDE_DIRS "include"
    REQUIRES
        common
        performance_recorder
        score_data
        score_engine
        input_source_manager
        usb_midi
)
