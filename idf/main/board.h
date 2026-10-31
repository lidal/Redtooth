#pragma once

#include <stdbool.h>
#include <stdint.h>

// === Virtual Boy controller (shift register) ===
#define VB_LATCH_PIN     5
#define VB_CLOCK_PIN     6
#define VB_DATA_PIN      8
#define POWER_SWITCH_PIN 7  // LOW = switched off

// Bit order of the shift register
enum {
    VB_R_DOWN = 0,
    VB_R_LEFT = 1,
    VB_SELECT = 2,
    VB_START = 3,
    VB_L_UP = 4,
    VB_L_DOWN = 5,
    VB_L_LEFT = 6,
    VB_L_RIGHT = 7,
    VB_R_RIGHT = 8,
    VB_R_UP = 9,
    VB_L_TRIG = 10,
    VB_R_TRIG = 11,
    VB_B = 12,
    VB_A = 13,
};

// Reads all buttons. Bit n is set while button n (VB_*) is pressed.
uint16_t vb_read_buttons(void);

static inline bool vb_pressed(uint16_t buttons, int button)
{
    return buttons & (1u << button);
}

// Pins, NeoPixel and battery gauge
void board_init(void);

// NeoPixel colour as 0xRRGGBB (0 = off)
void led_set(uint32_t rgb);

// Battery charge from the MAX17048 gauge. False if there is no gauge.
bool battery_percent(float *percent);

bool power_switch_off(void);

// Sleeps until the power switch is turned on again, which restarts the chip.
void board_deep_sleep(void);
