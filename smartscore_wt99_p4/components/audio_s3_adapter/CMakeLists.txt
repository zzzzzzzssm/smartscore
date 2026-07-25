idf_component_register(
    SRCS "src/audio_s3_adapter.c"
    INCLUDE_DIRS "include"
    REQUIRES s3_bus
)
