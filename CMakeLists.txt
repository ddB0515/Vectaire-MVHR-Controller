cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.10.0")

# Build variant. "display" (default): the NiceMCU C5 board with its touch screen, sdkconfig in the project folder.
# "headless": any ESP32 without a screen (no board/ui components, no LVGL); tools/idf.sh --headless <target> sets
# this together with the target and its own build folder, which also holds its sdkconfig.
set(VECTAIRE_VARIANT "display" CACHE STRING "display or headless")
if(VECTAIRE_VARIANT STREQUAL "headless")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.headless")
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
    set(EXCLUDE_COMPONENTS board ui)
    set(VECTAIRE_PROJECT vectaire_headless)
elseif(VECTAIRE_VARIANT STREQUAL "display")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.display")
    set(VECTAIRE_PROJECT vectaire_c5)
else()
    message(FATAL_ERROR "VECTAIRE_VARIANT must be display or headless, not '${VECTAIRE_VARIANT}'")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Read by main/CMakeLists.txt (component requirements) and by the code (#ifdef VECTAIRE_HEADLESS)
if(VECTAIRE_VARIANT STREQUAL "headless")
    idf_build_set_property(VECTAIRE_HEADLESS 1)
    idf_build_set_property(COMPILE_DEFINITIONS "VECTAIRE_HEADLESS=1" APPEND)
endif()

# The project name differs per variant, so OTA refuses an image of the other variant (ota.c check_image)
project(${VECTAIRE_PROJECT})
