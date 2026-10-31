#include "hid_device.h"

#include <string.h>

#include "ble_common.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "hid";

static const hid_device_config_t *s_cfg;
static uint8_t s_input[64];  // last input report, also returned to reads
static uint8_t s_output[16];
static size_t s_output_len;
static uint8_t s_battery = 100;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool s_subscribed;
static uint16_t s_report_map_handle, s_input_handle, s_battery_handle;

enum {
    ATTR_PNP_ID,
    ATTR_MANUFACTURER,
    ATTR_MODEL_NUMBER,
    ATTR_FIRMWARE_REVISION,
    ATTR_OUTPUT_REPORT,
    ATTR_OUTPUT_REPORT_REF,
    ATTR_REPORT_MAP,
    ATTR_INPUT_REPORT,
    ATTR_INPUT_REPORT_REF,
    ATTR_HID_INFO,
    ATTR_CONTROL_POINT,
    ATTR_PROTOCOL_MODE,
    ATTR_BATTERY_LEVEL,
    ATTR_BATTERY_FORMAT,
};

static int append(struct ble_gatt_access_ctxt *ctxt, const void *data, size_t len)
{
    return os_mbuf_append(ctxt->om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    bool read = ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR || ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC;

    switch ((int)(intptr_t)arg) {
    case ATTR_PNP_ID:
        return append(ctxt, s_cfg->pnp_id, sizeof(s_cfg->pnp_id));
    case ATTR_MANUFACTURER:
        return append(ctxt, s_cfg->manufacturer, strlen(s_cfg->manufacturer));
    case ATTR_MODEL_NUMBER:
        return append(ctxt, s_cfg->model_number, strlen(s_cfg->model_number));
    case ATTR_FIRMWARE_REVISION:
        return append(ctxt, s_cfg->firmware_revision, strlen(s_cfg->firmware_revision));
    case ATTR_REPORT_MAP:
        return append(ctxt, s_cfg->report_map, s_cfg->report_map_len);
    case ATTR_INPUT_REPORT:
        return append(ctxt, s_input, s_cfg->input_report_len);
    case ATTR_INPUT_REPORT_REF: {
        uint8_t ref[] = { s_cfg->input_report_id, 1 };  // report ID, type input
        return append(ctxt, ref, sizeof(ref));
    }
    case ATTR_OUTPUT_REPORT:
        if (read) return append(ctxt, s_output, s_output_len);
        // Rumble. The Virtual Boy pad has no motor; keep the value for reads.
        s_output_len = OS_MBUF_PKTLEN(ctxt->om);
        if (s_output_len > sizeof(s_output)) s_output_len = sizeof(s_output);
        os_mbuf_copydata(ctxt->om, 0, s_output_len, s_output);
        return 0;
    case ATTR_OUTPUT_REPORT_REF: {
        if (!read) return 0;
        uint8_t ref[] = { s_cfg->output_report_id, 2 };  // report ID, type output
        return append(ctxt, ref, sizeof(ref));
    }
    case ATTR_HID_INFO: {
        static const uint8_t info[] = { 0x11, 0x01, 0x00, 0x01 };  // HID 1.11, no country, remote wake
        return append(ctxt, info, sizeof(info));
    }
    case ATTR_CONTROL_POINT:
        return 0;  // suspend / exit suspend
    case ATTR_PROTOCOL_MODE: {
        if (!read) return 0;
        uint8_t mode = 1;  // report protocol
        return append(ctxt, &mode, 1);
    }
    case ATTR_BATTERY_LEVEL:
        return append(ctxt, &s_battery, 1);
    case ATTR_BATTERY_FORMAT: {
        // uint8, exponent 0, unit percentage, namespace Bluetooth SIG, no description
        static const uint8_t format[] = { 0x04, 0x00, 0xAD, 0x27, 0x01, 0x00, 0x00 };
        return append(ctxt, format, sizeof(format));
    }
    }
    return BLE_ATT_ERR_UNLIKELY;
}

#define ATTR(n) ((void *)(intptr_t)(n))

static struct ble_gatt_dsc_def s_output_ref_dsc[] = {
    {
        .uuid = BLE_UUID16_DECLARE(0x2908),
        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE | BLE_ATT_F_READ_ENC | BLE_ATT_F_WRITE_ENC,
        .access_cb = access_cb,
        .arg = ATTR(ATTR_OUTPUT_REPORT_REF),
    },
    { 0 },
};

static struct ble_gatt_dsc_def s_input_ref_dsc[] = {
    {
        .uuid = BLE_UUID16_DECLARE(0x2908),
        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
        .access_cb = access_cb,
        .arg = ATTR(ATTR_INPUT_REPORT_REF),
    },
    { 0 },
};

static struct ble_gatt_dsc_def s_battery_dsc[] = {
    {
        .uuid = BLE_UUID16_DECLARE(0x2904),
        .att_flags = BLE_ATT_F_READ,
        .access_cb = access_cb,
        .arg = ATTR(ATTR_BATTERY_FORMAT),
    },
    { 0 },
};

static const struct ble_gatt_chr_def s_dis_chrs[] = {
    { .uuid = BLE_UUID16_DECLARE(0x2A50), .flags = BLE_GATT_CHR_F_READ, .access_cb = access_cb, .arg = ATTR(ATTR_PNP_ID) },
    { .uuid = BLE_UUID16_DECLARE(0x2A29), .flags = BLE_GATT_CHR_F_READ, .access_cb = access_cb, .arg = ATTR(ATTR_MANUFACTURER) },
    { .uuid = BLE_UUID16_DECLARE(0x2A24), .flags = BLE_GATT_CHR_F_READ, .access_cb = access_cb, .arg = ATTR(ATTR_MODEL_NUMBER) },
    { .uuid = BLE_UUID16_DECLARE(0x2A26), .flags = BLE_GATT_CHR_F_READ, .access_cb = access_cb, .arg = ATTR(ATTR_FIRMWARE_REVISION) },
    { 0 },
};

// The 8BitDo adapter doesn't discover services: it reads the report map at
// handle 28 and turns on input notifications at handle 31, where a real Xbox
// pad has them. GAP and GATT take handles 1-13, so this order gives:
//   14     Device Information (PnP ID, manufacturer, model, firmware)
//   23     HID: 24-26 output report, 27/28 report map,
//          29/30 input report, 31 its CCCD, 32 its report reference,
//          then HID info, control point, protocol mode
//   39     Battery
// Without an output report (Android mode) everything after it moves up, which
// doesn't matter to hosts that discover services.
static const struct ble_gatt_chr_def s_hid_chrs[] = {
    {
        .uuid = BLE_UUID16_DECLARE(0x2A4D),
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                 BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC,
        .access_cb = access_cb,
        .arg = ATTR(ATTR_OUTPUT_REPORT),
        .descriptors = s_output_ref_dsc,
    },
    {
        .uuid = BLE_UUID16_DECLARE(0x2A4B),
        .flags = BLE_GATT_CHR_F_READ,
        .access_cb = access_cb,
        .arg = ATTR(ATTR_REPORT_MAP),
        .val_handle = &s_report_map_handle,
    },
    {
        .uuid = BLE_UUID16_DECLARE(0x2A4D),
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
        .access_cb = access_cb,
        .arg = ATTR(ATTR_INPUT_REPORT),
        .val_handle = &s_input_handle,
        .descriptors = s_input_ref_dsc,
    },
    { .uuid = BLE_UUID16_DECLARE(0x2A4A), .flags = BLE_GATT_CHR_F_READ, .access_cb = access_cb, .arg = ATTR(ATTR_HID_INFO) },
    { .uuid = BLE_UUID16_DECLARE(0x2A4C), .flags = BLE_GATT_CHR_F_WRITE_NO_RSP, .access_cb = access_cb, .arg = ATTR(ATTR_CONTROL_POINT) },
    {
        .uuid = BLE_UUID16_DECLARE(0x2A4E),
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .access_cb = access_cb,
        .arg = ATTR(ATTR_PROTOCOL_MODE),
    },
    { 0 },
};

static const struct ble_gatt_chr_def s_battery_chrs[] = {
    {
        .uuid = BLE_UUID16_DECLARE(0x2A19),
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .access_cb = access_cb,
        .arg = ATTR(ATTR_BATTERY_LEVEL),
        .val_handle = &s_battery_handle,
        .descriptors = s_battery_dsc,
    },
    { 0 },
};

static struct ble_gatt_svc_def s_services[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x180A), .characteristics = s_dis_chrs },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x1812), .characteristics = s_hid_chrs },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x180F), .characteristics = s_battery_chrs },
    { 0 },
};

static void advertise(void);

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
        if (ble_gap_conn_find(s_conn, &desc) == 0) {
            ESP_LOGI(TAG, "Connected to %s", ble_addr_str(desc.peer_ota_addr.val));
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected, reason 0x%x", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "Encryption status %d: encrypted=%d bonded=%d", event->enc_change.status,
                     desc.sec_state.encrypted, desc.sec_state.bonded);
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "Notifications for handle %d: %s", event->subscribe.attr_handle,
                 event->subscribe.cur_notify ? "on" : "off");
        if (event->subscribe.attr_handle == s_input_handle) s_subscribed = event->subscribe.cur_notify;
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // The host lost our keys and pairs again: forget the old ones.
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

static void advertise(void)
{
    // Like a real Xbox pad: flags, manufacturer data (Swift Pair), appearance and
    // HID service in the advertisement, name in the scan response.
    uint8_t adv[31], scan_rsp[31];
    size_t adv_len = 0, scan_rsp_len = 0;
    uint8_t flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    uint8_t appearance[] = { s_cfg->appearance & 0xFF, s_cfg->appearance >> 8 };
    uint8_t hid_uuid[] = { 0x12, 0x18 };

    adv_len = ble_ad_add(adv, adv_len, BLE_HS_ADV_TYPE_FLAGS, &flags, 1);
    if (s_cfg->adv_manufacturer_data_len) {
        adv_len = ble_ad_add(adv, adv_len, BLE_HS_ADV_TYPE_MFG_DATA, s_cfg->adv_manufacturer_data,
                             s_cfg->adv_manufacturer_data_len);
    }
    adv_len = ble_ad_add(adv, adv_len, BLE_HS_ADV_TYPE_APPEARANCE, appearance, sizeof(appearance));
    adv_len = ble_ad_add(adv, adv_len, BLE_HS_ADV_TYPE_COMP_UUIDS16, hid_uuid, sizeof(hid_uuid));
    scan_rsp_len = ble_ad_add(scan_rsp, scan_rsp_len, BLE_HS_ADV_TYPE_COMP_NAME, s_cfg->name, strlen(s_cfg->name));

    ble_advertise(adv, adv_len, scan_rsp, scan_rsp_len, gap_event, NULL);
}

static void configure(void)
{
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = s_cfg->secure_connections;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s_cfg->name);
    ble_svc_gap_device_appearance_set(s_cfg->appearance);

    if (!s_cfg->output_report_id) s_services[1].characteristics = s_hid_chrs + 1;
    ESP_ERROR_CHECK(ble_gatts_count_cfg(s_services));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(s_services));
}

static void on_sync(void)
{
    ESP_LOGI(TAG, "Report map at handle %d, input report %d, its notification switch %d",
             s_report_map_handle, s_input_handle, s_input_handle + 1);
    if (s_cfg->expected_report_map_handle && s_report_map_handle != s_cfg->expected_report_map_handle) {
        ESP_LOGE(TAG, "Report map should be at handle %d; the 8BitDo adapter won't work",
                 s_cfg->expected_report_map_handle);
    }
    advertise();
}

void hid_device_start(const hid_device_config_t *config)
{
    static const ble_mode_t mode = { .configure = configure, .on_sync = on_sync };
    s_cfg = config;
    ble_start(&mode);
}

bool hid_device_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

void hid_device_send(const void *report)
{
    memcpy(s_input, report, s_cfg->input_report_len);
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;
    if (!s_subscribed && !s_cfg->notify_unsubscribed) return;
    ble_notify(conn, s_input_handle, s_input, s_cfg->input_report_len);
}

void hid_device_set_battery(uint8_t percent)
{
    if (percent == s_battery) return;
    s_battery = percent;
    if (s_battery_handle) ble_gatts_chr_updated(s_battery_handle);  // notifies subscribers
}

void hid_device_disconnect(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}
