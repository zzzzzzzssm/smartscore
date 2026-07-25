idf_component_register(
    SRCS "test_s3_protocol.c"
    INCLUDE_DIRS "."
    REQUIRES unity s3_bus
)
