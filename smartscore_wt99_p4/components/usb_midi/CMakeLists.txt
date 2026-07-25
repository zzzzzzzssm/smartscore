idf_component_register(
    SRCS
        "src/usb_midi.c"
        "src/usb_midi_host.c"
        "src/usb_midi_parser.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        esp_timer
        usb
)
