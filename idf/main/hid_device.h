#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A BLE HID gamepad (HID over GATT with Device Information and Battery), used
// by the Android and Xbox modes.
typedef struct {
    const char *name;        // GAP device name, also in the scan response
    uint16_t appearance;
    uint8_t pnp_id[7];       // vendor ID source, vendor ID, product ID, version (little endian)
    const char *manufacturer;
    const char *model_number;
    const char *firmware_revision;
    const uint8_t *report_map;
    size_t report_map_len;
    uint8_t input_report_id;
    size_t input_report_len;
    uint8_t output_report_id;  // 0 = no output report
    bool secure_connections;   // pair with LE Secure Connections
    const uint8_t *adv_manufacturer_data;  // optional, added to the advertisement
    size_t adv_manufacturer_data_len;
    // Send input reports even when the host hasn't turned on notifications in
    // this connection (a host that thinks it's still subscribed from an earlier
    // bond never writes the CCCD again).
    bool notify_unsubscribed;
    uint16_t expected_report_map_handle;  // warn if the GATT table differs (0 = don't check)
} hid_device_config_t;

void hid_device_start(const hid_device_config_t *config);
bool hid_device_connected(void);
void hid_device_send(const void *report);  // input_report_len bytes
void hid_device_set_battery(uint8_t percent);
void hid_device_disconnect(void);
