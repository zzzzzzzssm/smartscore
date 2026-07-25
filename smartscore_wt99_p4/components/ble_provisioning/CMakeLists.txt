idf_component_register(
    SRCS
        "src/ble_provisioning.c"
        "src/ble_gatt_server.c"
        "src/ble_message_codec.c"
        "src/ble_notify.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        bt
        json
        esp_hw_support
        esp_hosted
        network_provisioning
        wifi_remote
)
