// Android mode: a generic BLE gamepad. Same buttons, Z/Rz axes and hat as the
// Arduino sketch (ESP32-BLE-CompositeHID with 10 buttons), plus X/Y for the
// left D-pad when Select + L makes it the left stick.
#include "board.h"
#include "hid_device.h"
#include "modes.h"
#include "pad.h"

static const uint8_t s_report_map[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,        // Generic Desktop, Game Pad, Application
    0x85, 0x01,                                //   report ID 1
    0x05, 0x09, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x19, 0x01, 0x29, 0x0A, 0x95, 0x0A, 0x81, 0x02,  // buttons 1-10
    0x75, 0x01, 0x95, 0x06, 0x81, 0x03,        //   6 bits padding
    0x05, 0x01, 0x09, 0x01, 0x16, 0x00, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x04,
    0xA1, 0x00, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x81, 0x02, 0xC0,  // X, Y, Z, Rz: 0-32767
    0xA1, 0x00, 0x05, 0x01, 0x09, 0x39, 0x15, 0x01, 0x25, 0x08, 0x35, 0x00, 0x46, 0x3B, 0x01,
    0x65, 0x12, 0x75, 0x08, 0x95, 0x01, 0x81, 0x42, 0xC0,  // hat switch 1-8, 0 = released
    0xC0,
};

typedef struct __attribute__((packed)) {
    uint16_t buttons;
    uint16_t x, y;  // left D-pad as the left stick
    uint16_t z;     // right D-pad up/down
    uint16_t rz;    // right D-pad left/right
    uint8_t hat;    // left D-pad as a D-pad
} android_report_t;

static const hid_device_config_t s_config = {
    .name = "Virtual Boy Controller",
    .appearance = 0x03C0,  // generic HID
    .pnp_id = { 0x01, 0x02, 0xE5, 0xAB, 0xBB, 0x10, 0x01 },  // VID 0xE502, PID 0xBBAB, version 0x0110
    .manufacturer = "DIY",
    .model_number = "1.0.0",
    .firmware_revision = "1.0.0",
    .report_map = s_report_map,
    .report_map_len = sizeof(s_report_map),
    .input_report_id = 1,
    .input_report_len = sizeof(android_report_t),
};

#define BUTTON(n) (1u << ((n) - 1))

// 1 = up, clockwise to 8 = up-left, 0 = released
static uint8_t hat(bool up, bool down, bool left, bool right)
{
    if (up) return left ? 8 : right ? 2 : 1;
    if (down) return left ? 6 : right ? 4 : 5;
    if (left) return 7;
    if (right) return 3;
    return 0;
}

static uint16_t axis(bool low, bool high)
{
    return low ? 0 : high ? 32767 : 16383;
}

static void update(uint16_t buttons)
{
    pad_update_toggles(buttons);

    // Select + B and Select + L are the toggles: B and L aren't sent while Select
    // is held, and Select isn't sent while it's used with them. Other Select
    // combos (Select + Start etc.) still reach the app as two buttons.
    bool select = vb_pressed(buttons, VB_SELECT);
    bool a = vb_pressed(buttons, VB_A);
    bool b = vb_pressed(buttons, VB_B) && !select;
    bool l = vb_pressed(buttons, VB_L_TRIG) && !select;
    bool swap = pad_swap_ab();

    android_report_t report = {
        .x = axis(false, false),
        .y = axis(false, false),
        .z = axis(vb_pressed(buttons, VB_R_UP), vb_pressed(buttons, VB_R_DOWN)),
        .rz = axis(vb_pressed(buttons, VB_R_LEFT), vb_pressed(buttons, VB_R_RIGHT)),
    };
    if (swap ? b : a) report.buttons |= BUTTON(1);
    if (swap ? a : b) report.buttons |= BUTTON(2);
    if (l) report.buttons |= BUTTON(5);
    if (vb_pressed(buttons, VB_R_TRIG)) report.buttons |= BUTTON(6);
    if (vb_pressed(buttons, VB_START)) report.buttons |= BUTTON(9);
    if (select && !vb_pressed(buttons, VB_B) && !vb_pressed(buttons, VB_L_TRIG)) report.buttons |= BUTTON(10);

    bool up = vb_pressed(buttons, VB_L_UP);
    bool down = vb_pressed(buttons, VB_L_DOWN);
    bool left = vb_pressed(buttons, VB_L_LEFT);
    bool right = vb_pressed(buttons, VB_L_RIGHT);
    if (pad_left_dpad_as_stick()) {
        report.x = axis(left, right);
        report.y = axis(up, down);
    } else {
        report.hat = hat(up, down, left, right);
    }
    hid_device_send(&report);
}

static void start(void)
{
    hid_device_start(&s_config);
}

const controller_mode_t android_mode = {
    .name = "Android",
    .color = 0x0000FF,
    .start = start,
    .connected = hid_device_connected,
    .update = update,
    .set_battery = hid_device_set_battery,
    .disconnect = hid_device_disconnect,
};
