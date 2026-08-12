idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        board_wt99
        wifi_remote
        network_provisioning
        ble_provisioning
        web_provisioning
        device_api
        speaker_service
        voice_assistant
        usb_midi
        s3_bus
        performance_recorder
        score_data
        scoring_service
        input_source_manager
        screen_adapter
        nvs_flash
)
