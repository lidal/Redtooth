// Switch 2 mode: a Nintendo Switch 2 Pro Controller over Bluetooth LE, talking
// to the console directly. GATT layout, pairing and report format are ported
// from ESP32-BLE5-NSController-Emulator (MIT, see LICENSE), which is based on
// https://github.com/ndeadly/switch2_controller_research.
//
// Pairing: open "Change Grip/Order" on the console. After that the controller
// reconnects (and wakes the console) by itself; use that screen only for pairing,
// input can freeze if a paired controller connects there.
// Needs the 5 ms connection interval patch (cmake/ble_interval_patch.cmake).

#include <string.h>

#include "ble_common.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "host/ble_hs.h"
#include "modes.h"
#include "nvs.h"
#include "pad.h"
#include "switch2_priv.h"

static const char *TAG = "switch2";

#define NVS_NAMESPACE      "PAIRING_PRO2"
#define REPORT_INTERVAL_MS 15

// Stick centres from the simulated factory calibration (switch2_cmd.c). Full
// deflection goes a little past the calibrated range (about 1500-1600) so it
// always reads as 100%. Assumes up is the higher value, as on Switch 1 pads.
#define LEFT_STICK_X  0x7B3
#define LEFT_STICK_Y  0x836
#define RIGHT_STICK_X 0x82C
#define RIGHT_STICK_Y 0x840
#define STICK_RANGE   1700

s2_pairing_t s2 = {
    .ctrl_addr = { 0x78, 0x81, 0x8c },  // Nintendo; the rest is random, made on first start
};

static bool s_paired;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_subscribed;  // the console turned on input report notifications
static volatile bool s_ready;       // and enabled the report features (command 0x0c/0x04)
static TimerHandle_t s_adv_timer;

// Latest input, written by the main loop, read by the report task
static portMUX_TYPE s_input_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_buttons[3];
static uint16_t s_sticks[4] = { LEFT_STICK_X, LEFT_STICK_Y, RIGHT_STICK_X, RIGHT_STICK_Y };

// ---- GATT ----
// https://github.com/ndeadly/switch2_controller_research/blob/master/bluetooth_interface.md
// The console expects these exact handles (no GAP/GATT services in front).

static uint16_t s_input_handle;     // 0x000e: input reports
static uint16_t s_command_handle;   // 0x0016: commands from the console
static uint16_t s_response_handle;  // 0x001e: answers to them

// 0x0001-0x0007
static const ble_uuid128_t SVC_1 = BLE_UUID128_INIT(0x80, 0xd2, 0x6b, 0xf9, 0x56, 0x19, 0x51, 0x8f, 0x30, 0x4e, 0x64, 0x19, 0x5d, 0xaf, 0xc5, 0x00);
static const ble_uuid128_t CHR_0003 = BLE_UUID128_INIT(0x81, 0xd2, 0x6b, 0xf9, 0x56, 0x19, 0x51, 0x8f, 0x30, 0x4e, 0x64, 0x19, 0x5d, 0xaf, 0xc5, 0x00);
static const ble_uuid128_t CHR_0005 = BLE_UUID128_INIT(0x82, 0xd2, 0x6b, 0xf9, 0x56, 0x19, 0x51, 0x8f, 0x30, 0x4e, 0x64, 0x19, 0x5d, 0xaf, 0xc5, 0x00);
static const ble_uuid128_t CHR_0007 = BLE_UUID128_INIT(0x83, 0xd2, 0x6b, 0xf9, 0x56, 0x19, 0x51, 0x8f, 0x30, 0x4e, 0x64, 0x19, 0x5d, 0xaf, 0xc5, 0x00);
// 0x0008-0x0032
static const ble_uuid128_t SVC_2 = BLE_UUID128_INIT(0xd0, 0x7f, 0xdf, 0x09, 0x8f, 0x11, 0x8f, 0x82, 0xad, 0x49, 0xfe, 0x89, 0xbe, 0xe9, 0x7d, 0xab);
static const ble_uuid128_t CHR_000A = BLE_UUID128_INIT(0xd2, 0x7f, 0xdf, 0x09, 0x8f, 0x11, 0x8f, 0x82, 0xad, 0x49, 0xfe, 0x89, 0xbe, 0xe9, 0x7d, 0xab);
static const ble_uuid128_t CHR_000E = BLE_UUID128_INIT(0xf8, 0xc0, 0xfc, 0x5f, 0x75, 0x32, 0x58, 0x82, 0x19, 0x46, 0x3e, 0xec, 0x6c, 0x86, 0x92, 0x74);
static const ble_uuid128_t CHR_0012 = BLE_UUID128_INIT(0x05, 0x2b, 0xf7, 0x31, 0x0c, 0x63, 0x39, 0xa9, 0x7d, 0x42, 0x58, 0x92, 0x51, 0x3f, 0x48, 0xcc);
static const ble_uuid128_t CHR_0014 = BLE_UUID128_INIT(0x05, 0xf0, 0xe5, 0x4f, 0xa5, 0x1e, 0x44, 0xaf, 0x6c, 0x4e, 0xb7, 0x8e, 0xc9, 0x4a, 0x9d, 0x64);
static const ble_uuid128_t CHR_0016 = BLE_UUID128_INIT(0x79, 0xb3, 0xe8, 0x09, 0x98, 0x6f, 0xaf, 0x8e, 0xb5, 0x40, 0x55, 0x69, 0x7e, 0xbc, 0xac, 0x3d);
static const ble_uuid128_t CHR_0018 = BLE_UUID128_INIT(0x8d, 0x9f, 0xf5, 0x5d, 0x3e, 0xd2, 0xf7, 0xa4, 0xf7, 0x4d, 0xae, 0xfd, 0x3d, 0x42, 0x47, 0x41);
static const ble_uuid128_t CHR_001A = BLE_UUID128_INIT(0x6a, 0x83, 0x11, 0xb1, 0x15, 0x53, 0x0a, 0xa2, 0x36, 0x4d, 0xd8, 0xd9, 0x61, 0xa9, 0x65, 0xc7);
static const ble_uuid128_t CHR_001E = BLE_UUID128_INIT(0xe0, 0x57, 0x76, 0xa7, 0x6b, 0x32, 0x49, 0xa5, 0x95, 0x4e, 0x78, 0x42, 0x7d, 0x9f, 0x6d, 0x50);
static const ble_uuid128_t CHR_0022 = BLE_UUID128_INIT(0x80, 0x2a, 0x6d, 0x40, 0x6f, 0xf8, 0x15, 0xab, 0x41, 0x42, 0x1c, 0x84, 0xd2, 0x69, 0xbd, 0xd3);
static const ble_uuid128_t CHR_0026 = BLE_UUID128_INIT(0xde, 0x7f, 0xdf, 0x09, 0x8f, 0x11, 0x8f, 0x82, 0xad, 0x49, 0xfe, 0x89, 0xbe, 0xe9, 0x7d, 0xab);
static const ble_uuid128_t CHR_002A = BLE_UUID128_INIT(0xdf, 0x7f, 0xdf, 0x09, 0x8f, 0x11, 0x8f, 0x82, 0xad, 0x49, 0xfe, 0x89, 0xbe, 0xe9, 0x7d, 0xab);
static const ble_uuid128_t CHR_002C = BLE_UUID128_INIT(0x06, 0x2b, 0xf7, 0x31, 0x0c, 0x63, 0x39, 0xa9, 0x7d, 0x42, 0x58, 0x92, 0x51, 0x3f, 0x48, 0xcc);
static const ble_uuid128_t CHR_002E = BLE_UUID128_INIT(0xf9, 0xc0, 0xfc, 0x5f, 0x75, 0x32, 0x58, 0x82, 0x19, 0x46, 0x3e, 0xec, 0x6c, 0x86, 0x92, 0x74);
static const ble_uuid128_t CHR_0032 = BLE_UUID128_INIT(0x80, 0xb3, 0xe8, 0x09, 0x98, 0x6f, 0xaf, 0x8e, 0xb5, 0x40, 0x55, 0x69, 0x7e, 0xbc, 0xac, 0x3d);
// Descriptors
static const ble_uuid128_t DSC_REPORT = BLE_UUID128_INIT(0xcb, 0x6e, 0x48, 0x80, 0xdf, 0x95, 0x57, 0x95, 0xee, 0x4d, 0x24, 0x5a, 0x10, 0x55, 0x9d, 0x67);
static const ble_uuid128_t DSC_NOTIFY = BLE_UUID128_INIT(0x79, 0xf9, 0xa4, 0xed, 0xbb, 0xe3, 0xd2, 0x9c, 0x5b, 0x49, 0x58, 0xf3, 0x8c, 0xdf, 0x46, 0xb7);

static uint16_t s_chr_0003_handle, s_chr_0007_handle;

// Reads of 0x0003 and 0x0007 (unknown values from a real controller), writes to 0x0005
static int svc1_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    static const uint8_t value_0003[] = { 0x04, 0x00, 0x05, 0x00, 0x01, 0x01, 0x00 };
    static const uint8_t value_0007[] = { 0x36, 0x80, 0x74, 0xee, 0xbb, 0x3d, 0x8e, 0x13 };

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const uint8_t *value = attr_handle == s_chr_0003_handle ? value_0003 : value_0007;
        size_t len = attr_handle == s_chr_0003_handle ? sizeof(value_0003) : sizeof(value_0007);
        return os_mbuf_append(ctxt->om, value, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return OS_MBUF_PKTLEN(ctxt->om) >= 2 ? 0 : BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

// Everything else is accepted and reads as empty
static int empty_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return 0;
}

// Writes without response: commands on 0x0016; rumble, firmware update, audio etc. are ignored
static int write_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || attr_handle != s_command_handle) return 0;

    static uint8_t cmd[512];
    static uint8_t rsp[S2_RSP_MAX];
    size_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len < S2_CMD_PREFIX_LEN + 8 || len - S2_CMD_PREFIX_LEN > sizeof(cmd)) {
        ESP_LOGE(TAG, "Command with unexpected length %d", (int)len);
        return BLE_ATT_ERR_UNLIKELY;
    }
    len -= S2_CMD_PREFIX_LEN;
    os_mbuf_copydata(ctxt->om, S2_CMD_PREFIX_LEN, len, cmd);

    size_t rsp_len;
    if (s2_cmd_process(cmd, len, rsp, &rsp_len) == 0) ble_notify(conn_handle, s_response_handle, rsp, rsp_len);
    return 0;
}

#define DSC(uuid_, flags_) { .uuid = &(uuid_).u, .att_flags = (flags_), .access_cb = empty_access }
#define NO_DSC { 0 }

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_1.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &CHR_0003.u, .flags = BLE_GATT_CHR_F_READ, .access_cb = svc1_access, .val_handle = &s_chr_0003_handle },
            { .uuid = &CHR_0005.u, .flags = BLE_GATT_CHR_F_WRITE, .access_cb = svc1_access },
            { .uuid = &CHR_0007.u, .flags = BLE_GATT_CHR_F_READ, .access_cb = svc1_access, .val_handle = &s_chr_0007_handle },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_2.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &CHR_000A.u, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .access_cb = empty_access,
                .descriptors = (struct ble_gatt_dsc_def[]) { DSC(DSC_REPORT, BLE_ATT_F_READ | BLE_ATT_F_WRITE), NO_DSC },
            },
            {
                .uuid = &CHR_000E.u, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .access_cb = empty_access,
                .val_handle = &s_input_handle,
                .descriptors = (struct ble_gatt_dsc_def[]) { DSC(DSC_REPORT, BLE_ATT_F_READ | BLE_ATT_F_WRITE), NO_DSC },
            },
            { .uuid = &CHR_0012.u, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP, .access_cb = write_access },  // rumble
            { .uuid = &CHR_0014.u, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP, .access_cb = write_access },
            {
                .uuid = &CHR_0016.u, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP, .access_cb = write_access,
                .val_handle = &s_command_handle,
            },
            { .uuid = &CHR_0018.u, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP, .access_cb = write_access },  // firmware update
            {
                .uuid = &CHR_001A.u, .flags = BLE_GATT_CHR_F_NOTIFY, .access_cb = empty_access,
                .descriptors = (struct ble_gatt_dsc_def[]) { DSC(DSC_NOTIFY, BLE_ATT_F_READ | BLE_ATT_F_WRITE), NO_DSC },
            },
            {
                .uuid = &CHR_001E.u, .flags = BLE_GATT_CHR_F_NOTIFY, .access_cb = empty_access,
                .val_handle = &s_response_handle,
                .descriptors = (struct ble_gatt_dsc_def[]) { DSC(DSC_NOTIFY, BLE_ATT_F_READ | BLE_ATT_F_WRITE), NO_DSC },
            },
            {
                .uuid = &CHR_0022.u, .flags = BLE_GATT_CHR_F_NOTIFY, .access_cb = empty_access,
                .descriptors = (struct ble_gatt_dsc_def[]) { DSC(DSC_NOTIFY, BLE_ATT_F_READ | BLE_ATT_F_WRITE), NO_DSC },
            },
            {
                .uuid = &CHR_0026.u, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .access_cb = empty_access,
                .descriptors = (struct ble_gatt_dsc_def[]) { DSC(DSC_REPORT, BLE_ATT_F_READ | BLE_ATT_F_WRITE), NO_DSC },
            },
            { .uuid = &CHR_002A.u, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP, .access_cb = write_access },
            { .uuid = &CHR_002C.u, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP, .access_cb = write_access },  // headset audio
            {
                .uuid = &CHR_002E.u, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .access_cb = empty_access,
                .descriptors = (struct ble_gatt_dsc_def[]) { DSC(DSC_REPORT, BLE_ATT_F_READ), NO_DSC },
            },
            { .uuid = &CHR_0032.u, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP, .access_cb = write_access },
            { 0 },
        },
    },
    { 0 },
};

// ---- Input reports ----
// https://github.com/ndeadly/switch2_controller_research/blob/master/hid_reports.md

typedef struct __attribute__((packed)) {
    uint8_t counter;
    uint8_t power_info;
    uint8_t buttons[3];
    uint8_t left_stick[3];   // two 12-bit values
    uint8_t right_stick[3];
    uint8_t unknown_0b;      // always 0x38?
    uint8_t unknown_0c;
    uint8_t headset;
    uint8_t motion_len;      // always 0x28
    uint8_t motion[0x28];
    uint8_t reserved[8];
} pro2_report_t;
_Static_assert(sizeof(pro2_report_t) == 63, "Pro Controller 2 input report is 63 bytes");

// Button bits: byte 0, byte 1, byte 2
#define BTN0_B       0x01
#define BTN0_A       0x02
#define BTN0_R       0x10
#define BTN0_PLUS    0x40
#define BTN1_DOWN    0x01
#define BTN1_RIGHT   0x02
#define BTN1_LEFT    0x04
#define BTN1_UP      0x08
#define BTN1_L       0x10
#define BTN1_MINUS   0x40
#define BTN2_HOME    0x01
#define BTN2_CAPTURE 0x02

static void pack_stick(uint8_t out[3], uint16_t x, uint16_t y)
{
    out[0] = x & 0xFF;
    out[1] = ((y & 0x0F) << 4) | ((x >> 8) & 0x0F);
    out[2] = (y >> 4) & 0xFF;
}

static void report_task(void *arg)
{
    pro2_report_t report = {
        .power_info = 0x20,
        .unknown_0b = 0x38,
        .motion_len = 0x28,
    };
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(REPORT_INTERVAL_MS));
        uint16_t conn = s_conn;
        if (conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed || !s_ready) {
            report.counter = 0;
            continue;
        }
        taskENTER_CRITICAL(&s_input_lock);
        memcpy(report.buttons, s_buttons, sizeof(report.buttons));
        pack_stick(report.left_stick, s_sticks[0], s_sticks[1]);
        pack_stick(report.right_stick, s_sticks[2], s_sticks[3]);
        taskEXIT_CRITICAL(&s_input_lock);
        report.counter++;
        if (os_msys_num_free() >= 5) ble_notify(conn, s_input_handle, &report, sizeof(report));
    }
}

static uint16_t stick(uint16_t centre, int8_t direction)
{
    int value = centre + direction * STICK_RANGE;
    return value < 0 ? 0 : value > 0xFFF ? 0xFFF : value;
}

static void update(uint16_t vb_buttons)
{
    switch_pad_t pad;
    pad_map_switch(vb_buttons, &pad);

    uint8_t buttons[3] = { 0 };
    if (pad.b) buttons[0] |= BTN0_B;
    if (pad.a) buttons[0] |= BTN0_A;
    if (pad.r) buttons[0] |= BTN0_R;
    if (pad.plus) buttons[0] |= BTN0_PLUS;
    if (pad.down) buttons[1] |= BTN1_DOWN;
    if (pad.right) buttons[1] |= BTN1_RIGHT;
    if (pad.left) buttons[1] |= BTN1_LEFT;
    if (pad.up) buttons[1] |= BTN1_UP;
    if (pad.l) buttons[1] |= BTN1_L;
    if (pad.minus) buttons[1] |= BTN1_MINUS;
    if (pad.home) buttons[2] |= BTN2_HOME;
    if (pad.capture) buttons[2] |= BTN2_CAPTURE;

    taskENTER_CRITICAL(&s_input_lock);
    memcpy(s_buttons, buttons, sizeof(s_buttons));
    s_sticks[0] = stick(LEFT_STICK_X, pad.lx);
    s_sticks[1] = stick(LEFT_STICK_Y, -pad.ly);
    s_sticks[2] = stick(RIGHT_STICK_X, pad.rx);
    s_sticks[3] = stick(RIGHT_STICK_Y, -pad.ry);
    taskEXIT_CRITICAL(&s_input_lock);
}

// ---- Pairing ----

// Hands the agreed key to NimBLE so the console's encryption request succeeds.
// The console uses EDIV and Rand 0.
static int store_pairing(void)
{
    struct ble_store_value_sec sec = {
        .peer_addr = { .type = BLE_ADDR_PUBLIC },
        .bond_count = 1,
        .key_size = S2_LTK_LEN,
        .ltk_present = 1,
        .authenticated = 1,
        .sc = 1,
    };
    memcpy(sec.peer_addr.val, s2.console_addr, S2_ADDR_LEN);
    memcpy(sec.ltk, s2.ltk_le, S2_LTK_LEN);
    int rc = ble_store_write_our_sec(&sec);
    if (rc == 0) rc = ble_store_write_peer_sec(&sec);
    if (rc != 0) ESP_LOGE(TAG, "Storing the pairing key failed, rc=%d", rc);
    return rc;
}

int s2_on_pairing_done(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, "BLE_LTK", s2.ltk, S2_LTK_LEN);
        if (err == ESP_OK) err = nvs_set_blob(nvs, "HOST_MAC", s2.console_addr, S2_ADDR_LEN);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving the pairing failed: %s", esp_err_to_name(err));
        return -1;
    }
    s_paired = true;
    ESP_LOGI(TAG, "Paired with the console %s", ble_addr_str(s2.console_addr));
    return store_pairing();
}

void s2_on_features_enabled(void)
{
    s_ready = true;
    ESP_LOGI(TAG, "Console ready for input");
}

// Our address (Nintendo prefix + random, made once) and any earlier pairing
static void load_pairing(void)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs));

    size_t len = S2_ADDR_LEN;
    if (nvs_get_blob(nvs, "CTRL_MAC", s2.ctrl_addr_le, &len) == ESP_OK && len == S2_ADDR_LEN) {
        s2_reverse(s2.ctrl_addr_le, s2.ctrl_addr, S2_ADDR_LEN);
    } else {
        esp_fill_random(s2.ctrl_addr + 3, 3);
        s2_reverse(s2.ctrl_addr, s2.ctrl_addr_le, S2_ADDR_LEN);
        nvs_set_blob(nvs, "CTRL_MAC", s2.ctrl_addr_le, S2_ADDR_LEN);
        nvs_commit(nvs);
    }

    size_t ltk_len = S2_LTK_LEN, addr_len = S2_ADDR_LEN;
    s_paired = nvs_get_blob(nvs, "BLE_LTK", s2.ltk, &ltk_len) == ESP_OK && ltk_len == S2_LTK_LEN &&
               nvs_get_blob(nvs, "HOST_MAC", s2.console_addr, &addr_len) == ESP_OK && addr_len == S2_ADDR_LEN;
    if (s_paired) s2_reverse(s2.ltk, s2.ltk_le, S2_LTK_LEN);
    nvs_close(nvs);
}

void switch2_forget_pairing(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_erase_all(nvs);  // also the address: the console sees a new controller
    nvs_commit(nvs);
    nvs_close(nvs);
}

// ---- Connection ----

static int gap_event(struct ble_gap_event *event, void *arg);

static void advertise(void)
{
    uint8_t mfg[26] = {
        0x53, 0x05,              // company: Nintendo
        0x01, 0x00, 0x03,        // unknown
        0x7E, 0x05,              // vendor ID: Nintendo
        0x69, 0x20,              // product ID: Pro Controller 2
        0x00, 0x01,              // unknown
        0x00,                    // 0x81: reconnect to (and wake) the paired console
        0, 0, 0, 0, 0, 0,        // paired console's address
        0x0F,                    // unknown
        0, 0, 0, 0, 0, 0, 0,
    };
    if (s_paired) {
        mfg[11] = 0x81;
        memcpy(&mfg[12], s2.console_addr, S2_ADDR_LEN);
    }
    uint8_t adv[31];
    uint8_t flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    size_t len = ble_ad_add(adv, 0, BLE_HS_ADV_TYPE_FLAGS, &flags, 1);
    len = ble_ad_add(adv, len, BLE_HS_ADV_TYPE_MFG_DATA, mfg, sizeof(mfg));
    ble_advertise(adv, len, NULL, 0, gap_event, NULL);
}

static void adv_timer_cb(TimerHandle_t timer)
{
    advertise();
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            advertise();
            return 0;
        }
        s_conn = event->connect.conn_handle;
        xTimerStop(s_adv_timer, 0);
        if (ble_gap_conn_find(s_conn, &desc) == 0) {
            memcpy(s2.console_addr, desc.peer_ota_addr.val, S2_ADDR_LEN);
            ESP_LOGI(TAG, "Connected to %s", ble_addr_str(s2.console_addr));
            struct ble_gap_upd_params params = {
                .itvl_min = 6,
                .itvl_max = desc.conn_itvl,
                .latency = 0,
                .supervision_timeout = desc.supervision_timeout,
            };
            ble_gap_update_params(s_conn, &params);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected, reason 0x%x; advertising again in 3 s", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        s_ready = false;
        xTimerReset(s_adv_timer, 0);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "Connection interval %d.%02d ms", desc.conn_itvl * 125 / 100, desc.conn_itvl * 125 % 100);
        }
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        *event->conn_update_req.self_params = *event->conn_update_req.peer_params;
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "Encryption status %d", event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_input_handle) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "Input reports %s", s_subscribed ? "on" : "off");
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", event->mtu.value);
        return 0;
    }
    return 0;
}

static void configure(void)
{
    ble_att_set_preferred_mtu(512);
    // Pairing happens over the command channel; plain bonding just stores the key
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_sc_only = 0;
    ble_hs_cfg.sm_sec_lvl = 2;
    ble_hs_cfg.sm_keypress = 0;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;

    ESP_ERROR_CHECK(ble_gatts_count_cfg(s_services));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(s_services));
}

static void on_sync(void)
{
    if (s_input_handle != 0x000e || s_command_handle != 0x0016 || s_response_handle != 0x001e) {
        ESP_LOGE(TAG, "GATT handles are off (input %d, command %d, response %d); the console won't work",
                 s_input_handle, s_command_handle, s_response_handle);
    }
    if (s_paired) store_pairing();
    ble_gap_set_prefered_default_le_phy(BLE_HCI_LE_PHY_2M_PREF_MASK, BLE_HCI_LE_PHY_2M_PREF_MASK);
    advertise();
}

static void start(void)
{
    static const ble_mode_t mode = { .configure = configure, .on_sync = on_sync };

    load_pairing();
    ESP_ERROR_CHECK(esp_iface_mac_addr_set(s2.ctrl_addr, ESP_MAC_BT));
    if (s_paired) {
        ESP_LOGI(TAG, "Paired with %s; reconnecting", ble_addr_str(s2.console_addr));
    } else {
        ESP_LOGI(TAG, "Not paired: open Change Grip/Order on the Switch 2");
    }
    s_adv_timer = xTimerCreate("s2_adv", pdMS_TO_TICKS(3000), pdFALSE, NULL, adv_timer_cb);
    ble_start(&mode);
    xTaskCreate(report_task, "s2_report", 4096, NULL, 4, NULL);
}

static bool connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

static void set_battery(uint8_t percent)
{
    // Where the Pro Controller 2 reports its battery isn't known yet
}

static void disconnect(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

const controller_mode_t switch2_mode = {
    .name = "Switch 2",
    .color = 0xFF0000,
    .start = start,
    .connected = connected,
    .update = update,
    .set_battery = set_battery,
    .disconnect = disconnect,
};
