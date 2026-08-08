cmake_minimum_required(VERSION 3.16)

# Carry FIRMWARE_VERSION into the app descriptor baked into the image, so the
# version of an image sitting in the *other* OTA slot can be read back at
# runtime (the OTA tab's "Stored" column, via esp_ota_get_partition_description).
# Without this, ESP-IDF fills the descriptor from "git describe", which says
# nothing about the per-unit firmware version.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/main/version.h" _fw_ver_line REGEX "FIRMWARE_VERSION")
string(REGEX MATCH "\"([^\"]+)\"" _fw_ver_match "${_fw_ver_line}")
set(PROJECT_VER "${CMAKE_MATCH_1}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(SlaveHonda)

# version.h is read at configure time above, so a version bump has to re-run
# CMake for the descriptor to follow it.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/main/version.h")
