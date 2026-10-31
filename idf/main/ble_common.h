#pragma once

#include <stddef.h>
#include <stdint.h>

#include "host/ble_gap.h"

// What a mode plugs into the shared NimBLE startup
typedef struct {
    void (*configure)(void);  // after the stack is initialised: security settings, GATT services
    void (*on_sync)(void);    // the stack is ready: start advertising
} ble_mode_t;

// Starts NimBLE. Set the Bluetooth address (esp_iface_mac_addr_set) before this.
void ble_start(const ble_mode_t *mode);

// Connectable, scannable advertising with legacy PDUs on extended advertising
// instance 0, with ready-made advertisement and scan response data. Does
// nothing if already advertising.
int ble_advertise(const uint8_t *adv, size_t adv_len, const uint8_t *scan_rsp, size_t scan_rsp_len,
                  ble_gap_event_fn *event_cb, void *event_arg);

// Appends an AD structure to advertising data, returns the new length
size_t ble_ad_add(uint8_t *buf, size_t len, uint8_t type, const void *data, size_t data_len);

// Sends a notification whether or not the peer has subscribed
int ble_notify(uint16_t conn_handle, uint16_t attr_handle, const void *data, size_t len);

// For logs: Bluetooth address as aa:bb:cc:dd:ee:ff from NimBLE's byte order
const char *ble_addr_str(const uint8_t addr[6]);
