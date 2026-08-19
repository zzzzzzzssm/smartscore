idf_component_register(
    SRCS "src/scoring_service.c"
    INCLUDE_DIRS "include"
    REQUIRES
        common
        performance_recorder
        practice_advice_service
        score_data
        score_engine
        input_source_manager
        s3_bus
        usb_midi
)
