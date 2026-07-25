set(SMARTMUSIC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../vendor/smartmusic_screen/SmartMusic")

idf_component_register(
    SRCS "${SMARTMUSIC_ROOT}/main/creator_mode.c"
    INCLUDE_DIRS
        "${SMARTMUSIC_ROOT}/main"
        "${SMARTMUSIC_ROOT}/components/usb_midi_input/include"
    REQUIRES
        app_font
        creator_core
        display_board_adapter
        esp_timer
        midi_parser
        music_display
        screen_legacy_storage
        ui_generated
)
