idf_component_register(
    SRCS
        "src/wifi_remote.c"
        "src/wifi_remote_events.c"
        "src/wifi_remote_status.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_event
        esp_netif
        esp_wifi
        esp_hosted
        esp_wifi_remote
)
