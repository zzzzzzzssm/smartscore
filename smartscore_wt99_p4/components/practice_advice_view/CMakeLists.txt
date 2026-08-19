idf_component_register(
    SRCS "src/practice_advice_view.c"
    INCLUDE_DIRS "include"
    REQUIRES
        app_font
        json
        lvgl
        practice_advice_service
)
