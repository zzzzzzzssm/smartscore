idf_component_register(
    SRCS "src/device_api.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_server
        json
        network_provisioning
        speaker_service
        usb_midi
        score_data
        scoring_service
        screen_adapter
        screen_legacy_storage
        input_source_manager
        heap
)
