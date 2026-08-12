idf_component_register(
    SRCS
        "src/compact_score.c"
        "src/compact_score_store.c"
    INCLUDE_DIRS "include"
    REQUIRES
        freertos
        json
)

