idf_component_register(
    SRCS
        "src/s3_bus.c"
        "src/s3_protocol.c"
        "src/s3_voice_node.c"
        "src/s3_camera_node.c"
    INCLUDE_DIRS "include"
    REQUIRES
        board_wt99
        driver
        esp_timer
        json
)
