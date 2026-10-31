#pragma once

#include <stdbool.h>
#include <stdint.h>

// One controller mode: what it pretends to be, over Bluetooth
typedef struct {
    const char *name;
    uint32_t color;  // NeoPixel colour, 0xRRGGBB
    void (*start)(void);
    bool (*connected)(void);
    // Called when the buttons change, and repeatedly for a few seconds after connecting
    void (*update)(uint16_t buttons);
    void (*set_battery)(uint8_t percent);
    void (*disconnect)(void);
} controller_mode_t;

extern const controller_mode_t android_mode;  // BLE gamepad
extern const controller_mode_t xbox_mode;     // Xbox controller, for the 8BitDo adapter
extern const controller_mode_t switch2_mode;  // Switch 2 Pro Controller

// Forgets the Switch 2 pairing (the next Switch 2 start pairs as a new controller)
void switch2_forget_pairing(void);
