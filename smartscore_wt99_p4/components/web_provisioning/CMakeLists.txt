idf_component_register(
    SRCS
        "src/web_provisioning.c"
        "src/web_provisioning_handlers.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        esp_http_server
        esp_netif
        esp_wifi
        json
        network_provisioning
        wifi_remote
)
