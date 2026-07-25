set(SMARTMUSIC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../vendor/smartmusic_screen/SmartMusic")

idf_component_register(
    SRCS "${SMARTMUSIC_ROOT}/components/midi_parser/midi_parser.c"
    INCLUDE_DIRS "${SMARTMUSIC_ROOT}/components/midi_parser"
    REQUIRES esp_timer
)
