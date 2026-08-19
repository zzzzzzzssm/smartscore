set(SMARTMUSIC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../vendor/smartmusic_screen/SmartMusic")
set(SMARTMUSIC_DISPLAY "${SMARTMUSIC_ROOT}/components/music_display")

idf_component_register(
    SRCS
        "${SMARTMUSIC_DISPLAY}/music_display.c"
        "${SMARTMUSIC_DISPLAY}/leland_score_view.c"
        "${SMARTMUSIC_DISPLAY}/staff_display_test.c"
        "${SMARTMUSIC_DISPLAY}/lv_font_LelandSMuFL_64.c"
        "${SMARTMUSIC_DISPLAY}/lv_font_LelandSMuFL_72.c"
        "${SMARTMUSIC_DISPLAY}/lv_font_LelandBrace_252.c"
    INCLUDE_DIRS "${SMARTMUSIC_DISPLAY}"
    REQUIRES
        app_font
        lvgl
        midi_parser
        musicxml_renderer
        ui_generated
        display_board_adapter
        esp_timer
        json
        creator_core
)
