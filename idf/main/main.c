/*
 *  Virtual Boy Controller to Bluetooth LE (ESP32-C6, ESP-IDF)
 *  ----------------------------------------------------------
 *  Three modes, chosen at power-on:
 *    - Android:  generic BLE gamepad ("Virtual Boy Controller").           Blue.
 *    - Xbox:     pretends to be an Xbox controller, for an 8BitDo Wireless
 *                USB Adapter 2 plugged into a Switch.                      Green.
 *    - Switch 2: pretends to be a Switch 2 Pro Controller, straight to the
 *                console.                                                  Red.
 *
 *  Choosing the mode: hold L while switching on -> Android,
 *                     hold R -> Xbox, hold A -> Switch 2,
 *                     hold nothing -> same mode as last time.
 *  The NeoPixel shows the mode colour for two seconds, then blinks in it while
 *  waiting for a connection and goes dark once connected.
 *  Each mode uses its own Bluetooth address, so pair each one separately.
 *
 *  All modes:
 *    Select + A, 3 s  -> clear all Bluetooth pairings and restart
 *    Select + L       -> toggle left D-pad between D-pad and left stick
 *    Select + B       -> swap A and B
 *  Both toggles are remembered across power-offs.
 *  Xbox and Switch 2 modes:
 *    Select + Start   -> HOME
 *    Select + R       -> Capture
 *
 *  Pairing: Xbox mode - hold the adapter's button until it blinks fast.
 *           Switch 2 mode - open Change Grip/Order on the console (only for the
 *           first pairing; afterwards the controller reconnects by itself).
 */

#include "board.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_store.h"
#include "modes.h"
#include "pad.h"
#include "settings.h"

static const char *TAG = "vbctrl";

#define RESET_HOLD_TIME_MS 3000
#define SETTLE_TIME_MS     3000
#define SETTLE_REPEAT_MS   250
#define BATTERY_PERIOD_MS  10000

// Indexes stored in the "mode" setting (0 and 1 match the Arduino sketch)
static const controller_mode_t *const s_modes[] = { &android_mode, &xbox_mode, &switch2_mode };

static const controller_mode_t *choose_mode(void)
{
    uint8_t stored = settings_get("mode", 0);
    if (stored >= sizeof(s_modes) / sizeof(s_modes[0])) stored = 0;
    uint8_t chosen = stored;

    uint16_t buttons = vb_read_buttons();
    if (vb_pressed(buttons, VB_L_TRIG)) chosen = 0;
    else if (vb_pressed(buttons, VB_R_TRIG)) chosen = 1;
    else if (vb_pressed(buttons, VB_A)) chosen = 2;

    if (chosen != stored) settings_set("mode", chosen);
    return s_modes[chosen];
}

static void clear_pairings_and_restart(void)
{
    ESP_LOGW(TAG, "Clearing all Bluetooth pairings and restarting...");
    ble_store_clear();
    switch2_forget_pairing();
    // New Xbox-mode address too, so hosts can't reuse anything they cached
    settings_set("addrgen", settings_get("addrgen", 0) + 1);
    esp_restart();
}

static void go_to_sleep(const controller_mode_t *mode)
{
    ESP_LOGI(TAG, "Switched off. Entering deep sleep.");
    if (mode->connected()) {
        mode->disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    board_deep_sleep();
}

void app_main(void)
{
    board_init();
    settings_init();
    pad_init();

    const controller_mode_t *mode = choose_mode();
    ESP_LOGI(TAG, "Virtual Boy Controller starting in %s mode", mode->name);
    led_set(mode->color);
    vTaskDelay(pdMS_TO_TICKS(2000));
    led_set(0);

    mode->start();
    ESP_LOGI(TAG, "Hold Select + A for 3 seconds to clear all pairings.");

    uint16_t last_sent = 0xFFFF;
    bool was_connected = false;
    bool led_lit = false;
    int64_t connected_at = 0, last_repeat = 0, reset_held_since = 0;
    int64_t last_battery = -BATTERY_PERIOD_MS;

    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;

        if (power_switch_off()) go_to_sleep(mode);

        float percent;
        if (now - last_battery >= BATTERY_PERIOD_MS) {
            last_battery = now;
            if (battery_percent(&percent)) {
                ESP_LOGI(TAG, "Battery %.1f %%", percent);
                mode->set_battery(percent > 100 ? 100 : percent < 0 ? 0 : (uint8_t)(percent + 0.5f));
            }
        }

        uint16_t buttons = vb_read_buttons();

        if (vb_pressed(buttons, VB_SELECT) && vb_pressed(buttons, VB_A)) {
            if (!reset_held_since) reset_held_since = now;
            else if (now - reset_held_since > RESET_HOLD_TIME_MS) clear_pairings_and_restart();
        } else {
            reset_held_since = 0;
        }

        // Send on changes. For the first few seconds after connecting, also repeat
        // the current state: the host may only subscribe once pairing has finished.
        bool connected = mode->connected();
        if (connected && !was_connected) connected_at = now;
        bool repeat = connected && now - connected_at < SETTLE_TIME_MS && now - last_repeat >= SETTLE_REPEAT_MS;
        if (repeat) last_repeat = now;
        if (buttons != last_sent || (connected && !was_connected) || repeat) {
            mode->update(buttons);
            last_sent = buttons;
        }
        was_connected = connected;

        // Short flash in the mode colour once a second while waiting for a connection
        bool lit = !connected && now % 1000 < 100;
        if (lit != led_lit) {
            led_set(lit ? mode->color : 0);
            led_lit = lit;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
