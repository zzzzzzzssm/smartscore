idf_component_register(
    SRCS
        "src/board_wt99.c"
        "src/board_audio.c"
        "src/board_sdcard.c"
    INCLUDE_DIRS "include"
    REQUIRES
        driver
        esp_codec_dev
        fatfs
        sdmmc
        esp_driver_sdmmc
)
