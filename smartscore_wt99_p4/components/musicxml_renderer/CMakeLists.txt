set(SMARTMUSIC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../vendor/smartmusic_screen/SmartMusic")

idf_component_register(
    SRCS
        "${SMARTMUSIC_ROOT}/components/musicxml_renderer/music_model.c"
        "${SMARTMUSIC_ROOT}/components/musicxml_renderer/midi_notation.c"
        "${SMARTMUSIC_ROOT}/components/musicxml_renderer/musicxml_parser.c"
        "${SMARTMUSIC_ROOT}/components/musicxml_renderer/smufl_glyphs.c"
        "${SMARTMUSIC_ROOT}/components/musicxml_renderer/music_layout.c"
    INCLUDE_DIRS "${SMARTMUSIC_ROOT}/components/musicxml_renderer/include"
    REQUIRES expat midi_parser
)
