# ESP-IDF version of the Virtual Boy controller firmware, with three modes:
# Android (BLE gamepad), Xbox (for the 8BitDo adapter) and Switch 2 Pro Controller.
# Build with ESP-IDF 6.1:  idf.py build
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(vb_controller)

# The Switch 2 needs a 5 ms connection interval; link a patched copy of the
# ESP32-C6 Bluetooth controller library that accepts it.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/ble_interval_patch.cmake")
