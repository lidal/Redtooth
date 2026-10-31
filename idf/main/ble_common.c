#include "ble_common.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

void ble_store_config_init(void);

static const char *TAG = "ble";
static const ble_mode_t *s_mode;

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "Bluetooth host reset, reason %d", reason);
}

static void on_sync(void)
{
    ESP_ERROR_CHECK(ble_hs_util_ensure_addr(0));
    uint8_t addr[6];
    if (ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, addr, NULL) == 0) {
        ESP_LOGI(TAG, "Bluetooth address %s", ble_addr_str(addr));
    }
    s_mode->on_sync();
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_start(const ble_mode_t *mode)
{
    s_mode = mode;
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    mode->configure();
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}

static int set_data(int (*setter)(uint8_t, struct os_mbuf *), const uint8_t *data, size_t len)
{
    struct os_mbuf *om = os_msys_get_pkthdr(len, 0);
    if (!om) return BLE_HS_ENOMEM;
    if (os_mbuf_append(om, data, len) != 0) {
        os_mbuf_free_chain(om);
        return BLE_HS_ENOMEM;
    }
    return setter(0, om);  // takes the mbuf
}

int ble_advertise(const uint8_t *adv, size_t adv_len, const uint8_t *scan_rsp, size_t scan_rsp_len,
                  ble_gap_event_fn *event_cb, void *event_arg)
{
    if (ble_gap_ext_adv_active(0)) return 0;

    struct ble_gap_ext_adv_params params = {
        .connectable = 1,
        .scannable = 1,
        .legacy_pdu = 1,
        .own_addr_type = BLE_OWN_ADDR_PUBLIC,
        .primary_phy = BLE_HCI_LE_PHY_1M,
        .secondary_phy = BLE_HCI_LE_PHY_1M,
        .itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN,
        .itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MIN,
        .channel_map = BLE_GAP_ADV_DFLT_CHANNEL_MAP,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
    };
    int rc = ble_gap_ext_adv_configure(0, &params, NULL, event_cb, event_arg);
    if (rc == 0) rc = set_data(ble_gap_ext_adv_set_data, adv, adv_len);
    if (rc == 0 && scan_rsp_len) rc = set_data(ble_gap_ext_adv_rsp_set_data, scan_rsp, scan_rsp_len);
    if (rc == 0) rc = ble_gap_ext_adv_start(0, 0, 0);
    if (rc != 0) ESP_LOGE(TAG, "Advertising failed, rc=%d", rc);
    return rc;
}

size_t ble_ad_add(uint8_t *buf, size_t len, uint8_t type, const void *data, size_t data_len)
{
    buf[len++] = data_len + 1;
    buf[len++] = type;
    memcpy(buf + len, data, data_len);
    return len + data_len;
}

int ble_notify(uint16_t conn_handle, uint16_t attr_handle, const void *data, size_t len)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return BLE_HS_ENOMEM;
    return ble_gatts_notify_custom(conn_handle, attr_handle, om);  // takes the mbuf
}

const char *ble_addr_str(const uint8_t addr[6])
{
    static char str[18];
    snprintf(str, sizeof(str), "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    return str;
}
