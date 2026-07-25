idf_component_register(
    SRCS
        "music_model.c"
        "musicxml_parser.c"
        "smufl_glyphs.c"
        "music_layout.c"
    INCLUDE_DIRS "include"
    REQUIRES expat
)
