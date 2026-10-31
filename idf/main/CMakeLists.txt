idf_component_register(
    SRCS "main.c" "board.c" "settings.c" "pad.c" "ble_common.c" "hid_device.c"
         "android.c" "xbox.c" "switch2/switch2.c" "switch2/switch2_cmd.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bt nvs_flash mbedtls esp_timer esp_driver_gpio esp_driver_rmt esp_driver_i2c)
