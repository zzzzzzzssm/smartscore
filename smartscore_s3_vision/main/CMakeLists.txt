idf_component_register(
    SRCS
        "app_main.cpp"
        "camera_driver.cpp"
        "sd_storage.cpp"
        "hand_ai.cpp"
        "point_direction_ai.cpp"
        "ai_worker.cpp"
        "gesture_engine.cpp"
        "p4_control_link.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_timer
        esp_app_format
        esp_psram
        fatfs
        esp_driver_sdmmc
        esp_driver_uart
        sdmmc
        esp-dl
)

idf_build_get_property(component_targets __COMPONENT_TARGETS)
if ("___idf_espressif__esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir espressif__esp-dl COMPONENT_DIR)
elseif ("___idf_esp-dl" IN_LIST component_targets)
    idf_component_get_property(espdl_dir esp-dl COMPONENT_DIR)
else()
    message(FATAL_ERROR "ESP-DL component target was not found")
endif()
set(cmake_dir ${espdl_dir}/fbs_loader/cmake)
include(${cmake_dir}/utilities.cmake)
target_add_aligned_binary_data(
    ${COMPONENT_LIB}
    "${CMAKE_CURRENT_LIST_DIR}/models/point_direction.espdl"
    BINARY
    RENAME_TO point_direction_espdl
)
