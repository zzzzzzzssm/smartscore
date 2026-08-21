idf_component_register(
    SRCS
        "music_model.c"
        "midi_notation.c"
        "musicxml_parser.c"
        "smufl_glyphs.c"
        "music_layout.c"
    INCLUDE_DIRS "include"
    REQUIRES expat midi_parser
)
