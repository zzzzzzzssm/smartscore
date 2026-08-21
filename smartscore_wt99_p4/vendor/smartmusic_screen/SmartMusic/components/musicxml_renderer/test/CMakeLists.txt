idf_component_register(
    SRCS
        "test_musicxml_renderer.c"
        "test_midi_notation.c"
        "test_midi_numbered_octaves.c"
        "test_music_layout_columns.c"
    INCLUDE_DIRS "."
    REQUIRES unity musicxml_renderer
)
