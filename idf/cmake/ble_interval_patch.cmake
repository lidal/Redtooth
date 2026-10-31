# The Switch 2 runs its controller link at a 5 ms connection interval. That is
# below the 7.5 ms minimum in the Bluetooth spec, and the ESP32-C6 controller
# library (libble_app.a, closed source) rejects it: the console connects but
# ignores every input report. This copies the library into the build folder,
# patches the copy to accept 5 ms (tools/patch_ble_lib.py) and links that
# instead. The library in the ESP-IDF install is left untouched.

idf_build_get_property(_ble_patch_target IDF_TARGET)
if(NOT _ble_patch_target STREQUAL "esp32c6")
    message(FATAL_ERROR "The connection interval patch only supports the ESP32-C6 (target is ${_ble_patch_target})")
endif()
if(NOT TARGET libble_app)
    message(FATAL_ERROR "ESP-IDF did not create the libble_app library target; is Bluetooth (NimBLE) enabled?")
endif()

idf_build_get_property(_ble_patch_python PYTHON)
get_target_property(_ble_patch_source libble_app IMPORTED_LOCATION)
set(_ble_patch_script "${CMAKE_CURRENT_LIST_DIR}/../tools/patch_ble_lib.py")
set(_ble_patch_output "${CMAKE_BINARY_DIR}/ble_patch/libble_app.a")

execute_process(
    COMMAND "${_ble_patch_python}" "${_ble_patch_script}"
            --ar "${CMAKE_AR}" --objdump "${CMAKE_OBJDUMP}"
            "${_ble_patch_source}" "${_ble_patch_output}"
    RESULT_VARIABLE _ble_patch_result
    OUTPUT_VARIABLE _ble_patch_stdout
    ERROR_VARIABLE _ble_patch_stderr)
if(NOT _ble_patch_result EQUAL 0)
    message(FATAL_ERROR "Patching ${_ble_patch_source} failed:\n${_ble_patch_stdout}${_ble_patch_stderr}")
endif()
message(STATUS "${_ble_patch_stdout}")

set_property(TARGET libble_app PROPERTY IMPORTED_LOCATION "${_ble_patch_output}")
# Patch again if the library or the script changes
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ble_patch_source}" "${_ble_patch_script}")
