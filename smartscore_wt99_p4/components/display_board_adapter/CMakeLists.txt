set(SMARTMUSIC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../vendor/smartmusic_screen/SmartMusic")
set(SMARTMUSIC_BSP "${SMARTMUSIC_ROOT}/components/wt99p4c5_s1_board")
set(SMARTMUSIC_BSP_SOURCE "${SMARTMUSIC_BSP}/wt99p4c5_s1_board.c")

idf_component_register(
    SRCS
        "${SMARTMUSIC_BSP_SOURCE}"
        "src/i2c_share.c"
    INCLUDE_DIRS "${SMARTMUSIC_BSP}/include"
    PRIV_INCLUDE_DIRS "${SMARTMUSIC_BSP}/priv_include"
    REQUIRES
        driver
        vfs
        fatfs
        esp_eth
        esp_netif
        esp_event
        esp_lcd
        usb
        spiffs
        esp_lcd_ek79007
        esp_lcd_touch_gt911
        esp_lvgl_port
        lvgl
)

# The touch controller and the existing ES8311 service share I2C1 on GPIO7/8.
# Redirect only the immutable vendor BSP translation unit to a bus-sharing
# wrapper; the current audio component remains untouched.
set_source_files_properties(
    "${SMARTMUSIC_BSP_SOURCE}"
    PROPERTIES COMPILE_DEFINITIONS
        "i2c_new_master_bus=screen_i2c_new_master_bus;i2c_del_master_bus=screen_i2c_del_master_bus"
)
