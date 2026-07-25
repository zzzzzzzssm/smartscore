idf_component_register(
    SRCS "src/network_diagnostics.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server json network_provisioning
)
