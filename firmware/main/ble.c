/* ble.c — NimBLE GATT 서버 (UART 스타일 RX/TX). ESP-IDF v5.5 */
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble.h"
#include "deck_io.h"

static const char *TAG = "ble";

static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t RX_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t TX_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t tx_handle = 0;
static bool tx_subscribed = false;
static uint8_t own_addr_type;

bool ble_connected(void) { return conn_handle != BLE_HS_CONN_HANDLE_NONE && tx_subscribed; }

/* ---------- GATT ---------- */
static int rx_write_cb(uint16_t ch, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    static uint8_t buf[600];
    if (len > sizeof(buf)) len = sizeof(buf);
    uint16_t out = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, &out) == 0 && out > 0)
        deck_rx_push(buf, out);
    return 0;
}

static int tx_access_cb(uint16_t ch, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return 0;   /* notify 전용, 읽기는 빈 값 */
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &RX_UUID.u, .access_cb = rx_write_cb,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &TX_UUID.u, .access_cb = tx_access_cb, .val_handle = &tx_handle,
              .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ },
            { 0 }
        },
    },
    { 0 }
};

/* ---------- 송신 ---------- */
void ble_send(const char *buf, int len)
{
    if (!ble_connected() || len <= 0) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
    if (!om) return;
    int rc = ble_gatts_notify_custom(conn_handle, tx_handle, om);
    if (rc != 0) ESP_LOGW(TAG, "notify rc=%d", rc);
}

/* ---------- GAP ---------- */
static void advertise(void);

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            conn_handle = ev->connect.conn_handle;
            tx_subscribed = false;
            ESP_LOGI(TAG, "connected");
            /* 빠른 연결 간격 요청 (7.5~15ms) — 아트 전송 속도 */
            struct ble_gap_upd_params p = {
                .itvl_min = 6, .itvl_max = 12, .latency = 0,
                .supervision_timeout = 400, .min_ce_len = 0, .max_ce_len = 0 };
            ble_gap_update_params(conn_handle, &p);
            deck_rx_push((const uint8_t *)"T:BLE|앱 연결됨|\n", strlen("T:BLE|앱 연결됨|\n"));
        } else {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected reason=%d", ev->disconnect.reason);
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        tx_subscribed = false;
        advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == tx_handle) {
            tx_subscribed = ev->subscribe.cur_notify;
            ESP_LOGI(TAG, "notify %s", tx_subscribed ? "on" : "off");
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu %d", ev->mtu.value);
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    default:
        return 0;
    }
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    fields.uuids128 = (ble_uuid128_t *)&SVC_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        /* 이름+UUID 가 31바이트를 넘으면 UUID 는 scan response 로 */
        fields.uuids128 = NULL; fields.num_uuids128 = 0;
        ble_gap_adv_set_fields(&fields);
        struct ble_hs_adv_fields rsp = {0};
        rsp.uuids128 = (ble_uuid128_t *)&SVC_UUID; rsp.num_uuids128 = 1; rsp.uuids128_is_complete = 1;
        ble_gap_adv_rsp_set_fields(&rsp);
    }
    struct ble_gap_adv_params adv = {0};
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "adv start rc=%d", rc);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    ESP_LOGI(TAG, "advertising as %s", DECK_BLE_NAME);
    advertise();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "reset reason=%d", reason); }

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_start(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs));
    ble_svc_gap_device_name_set(DECK_BLE_NAME);
    ble_att_set_preferred_mtu(517);
    nimble_port_freertos_init(host_task);
}
