idf_component_register(
    SRCS
        "src/score_data.c"
        "src/score_json_parser.c"
    INCLUDE_DIRS "include"
    REQUIRES
        common
        heap
        json
)
