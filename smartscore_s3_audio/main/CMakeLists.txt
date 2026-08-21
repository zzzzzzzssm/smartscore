idf_component_register(
    SRCS
        "main.c"
        "es7210_capture.c"
        "audio_preprocess.c"
        "adaptive_input_control.c"
        "dual_mic_selector.c"
        "yin_detector.c"
        "low_note_detector.c"
        "chord_detector.c"
        "piano_note_tracker.c"
        "melody_gate.c"
        "music_classifier.c"
        "music_detector.c"
        "music_uart_link.c"
        "note_utils.c"
        "diagnostics.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer esp_mm esp_psram freertos
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror=return-type)
