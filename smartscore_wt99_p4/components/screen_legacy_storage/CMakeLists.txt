set(SMARTMUSIC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../vendor/smartmusic_screen/SmartMusic")
set(SMARTMUSIC_STORAGE "${SMARTMUSIC_ROOT}/components/smartscore_core")
set(SMARTMUSIC_STORAGE_SOURCE "${SMARTMUSIC_STORAGE}/score_storage.c")

idf_component_register(
    SRCS
        "${SMARTMUSIC_STORAGE_SOURCE}"
        "src/legacy_score_json_bridge.c"
    INCLUDE_DIRS "${SMARTMUSIC_STORAGE}"
    REQUIRES
        midi_parser
        screen_score_bridge
        json
        spiffs
)

set_source_files_properties(
    "${SMARTMUSIC_STORAGE_SOURCE}"
    PROPERTIES COMPILE_DEFINITIONS
        "score_json_parse_and_store=legacy_score_json_parse_and_store;score_json_parse_and_store_with_tempo=legacy_score_json_parse_and_store_with_tempo"
)
