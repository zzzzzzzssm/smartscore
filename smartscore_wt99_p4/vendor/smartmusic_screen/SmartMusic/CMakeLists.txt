# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Audio capture and analysis run on the external audio processor, not on ESP32-P4.
set(EXCLUDE_COMPONENTS audio_capture audio_session)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(P4C5S1_SmartScreen)
