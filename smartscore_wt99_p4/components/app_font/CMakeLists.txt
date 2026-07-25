set(SMARTMUSIC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../vendor/smartmusic_screen/SmartMusic")

idf_component_register(
    SRCS "${SMARTMUSIC_ROOT}/components/app_font/app_font.c"
    INCLUDE_DIRS "${SMARTMUSIC_ROOT}/components/app_font"
    REQUIRES heap lvgl
)
