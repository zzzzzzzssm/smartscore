idf_component_register(
    SRCS
        "note_utils.c"
        "score_engine.c"
        "score_json_parser.c"
        "score_storage.c"
        "app_state.c"
    INCLUDE_DIRS "."
    REQUIRES
        json
        esp_timer
        midi_parser
        spiffs
)
