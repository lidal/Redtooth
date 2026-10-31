#include "pad.h"

#include "board.h"
#include "esp_log.h"
#include "settings.h"

static const char *TAG = "pad";

static bool s_left_dpad_as_stick;
static bool s_swap_ab;
static bool s_last_l, s_last_b;

void pad_init(void)
{
    s_left_dpad_as_stick = settings_get("lstick", 0);
    s_swap_ab = settings_get("swapab", 0);
    ESP_LOGI(TAG, "Left D-pad -> %s (Select+L to change)", s_left_dpad_as_stick ? "left stick" : "D-pad");
    ESP_LOGI(TAG, "A and B %s (Select+B to change)", s_swap_ab ? "swapped" : "not swapped");
}

static int8_t axis(bool negative, bool positive)
{
    return negative ? -1 : positive ? 1 : 0;
}

void pad_update_toggles(uint16_t buttons)
{
    bool select = vb_pressed(buttons, VB_SELECT);
    bool l = vb_pressed(buttons, VB_L_TRIG);
    bool b = vb_pressed(buttons, VB_B);

    if (select && l && !s_last_l) {
        s_left_dpad_as_stick = !s_left_dpad_as_stick;
        settings_set("lstick", s_left_dpad_as_stick);
        ESP_LOGI(TAG, "Left D-pad -> %s", s_left_dpad_as_stick ? "left stick" : "D-pad");
    }
    if (select && b && !s_last_b) {
        s_swap_ab = !s_swap_ab;
        settings_set("swapab", s_swap_ab);
        ESP_LOGI(TAG, "A and B %s", s_swap_ab ? "swapped" : "not swapped");
    }
    s_last_l = l;
    s_last_b = b;
}

bool pad_left_dpad_as_stick(void)
{
    return s_left_dpad_as_stick;
}

bool pad_swap_ab(void)
{
    return s_swap_ab;
}

void pad_map_switch(uint16_t buttons, switch_pad_t *pad)
{
    bool select = vb_pressed(buttons, VB_SELECT);
    bool start = vb_pressed(buttons, VB_START);
    bool l = vb_pressed(buttons, VB_L_TRIG);
    bool r = vb_pressed(buttons, VB_R_TRIG);
    bool a = vb_pressed(buttons, VB_A);
    bool b = vb_pressed(buttons, VB_B);

    pad_update_toggles(buttons);

    // While Select is held it acts as a modifier, so its partners are not sent.
    bool vb_a = a && !select;
    bool vb_b = b && !select;
    *pad = (switch_pad_t){
        .a = s_swap_ab ? vb_b : vb_a,
        .b = s_swap_ab ? vb_a : vb_b,
        .l = l && !select,
        .r = r && !select,
        .plus = start && !select,
        .minus = select && !start && !l && !r && !a && !b,
        .home = select && start,
        .capture = select && r,
        .rx = axis(vb_pressed(buttons, VB_R_LEFT), vb_pressed(buttons, VB_R_RIGHT)),
        .ry = axis(vb_pressed(buttons, VB_R_UP), vb_pressed(buttons, VB_R_DOWN)),
    };

    bool up = vb_pressed(buttons, VB_L_UP);
    bool down = vb_pressed(buttons, VB_L_DOWN);
    bool left = vb_pressed(buttons, VB_L_LEFT);
    bool right = vb_pressed(buttons, VB_L_RIGHT);
    if (s_left_dpad_as_stick) {
        pad->lx = axis(left, right);
        pad->ly = axis(up, down);
    } else {
        // Opposite directions cancel out
        pad->up = up && !down;
        pad->down = down && !up;
        pad->left = left && !right;
        pad->right = right && !left;
    }
}
