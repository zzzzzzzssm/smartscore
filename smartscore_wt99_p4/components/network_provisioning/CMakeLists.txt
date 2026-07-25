idf_component_register(
    SRCS
        "src/network_provisioning.c"
        "src/network_credentials.c"
        "src/network_scan_events.c"
        "src/network_state_machine.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        nvs_flash
        wifi_remote
)
