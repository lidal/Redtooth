#pragma once

#include <stdbool.h>
#include <stdint.h>

// What the Switch gets from the Virtual Boy buttons, for the two Switch modes
// (Xbox through the 8BitDo adapter, and Switch 2 Pro Controller):
//   A, B, L, R, Start = Plus, Select alone = Minus
//   Select + Start = HOME, Select + R = Capture
//   Select + L toggles the left D-pad between D-pad and left stick (all modes)
//   Select + B swaps A and B (all modes)
//   right D-pad = right stick
typedef struct {
    bool a, b, l, r, plus, minus, home, capture;
    bool up, down, left, right;  // D-pad
    int8_t lx, ly, rx, ry;       // sticks: -1, 0 or 1 (y: -1 = up)
} switch_pad_t;

// Loads the Select + L / Select + B toggles
void pad_init(void);

// Handles the toggles, which apply in every mode: Select + L (left D-pad as
// D-pad or left stick) and Select + B (swap A and B). Call on every button change.
void pad_update_toggles(uint16_t buttons);
bool pad_left_dpad_as_stick(void);
bool pad_swap_ab(void);

// Call on every button change: also handles the toggles.
void pad_map_switch(uint16_t buttons, switch_pad_t *pad);
