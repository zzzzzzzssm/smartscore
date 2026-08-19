idf_component_register(
    SRCS
        "src/practice_advice_service.c"
        "src/practice_advice_state.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        deepseek_advice
        esp_hw_support
        heap
        network_provisioning
)
