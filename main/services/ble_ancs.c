#include "ble_ancs.h"
#include "audio_codec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>

static const char *TAG = "ble_ancs";

#define BLE_DEVICE_NAME "ESP32-C6-Clock"

// Apple Notification Center Service (ANCS) 128-bit UUIDs (little-endian)
static const ble_uuid128_t ANCS_SVC_UUID = BLE_UUID128_INIT(
    0xd0, 0x00, 0x2d, 0x12, 0x1e, 0x4b, 0x0f, 0xa4,
    0x99, 0x4e, 0xce, 0xb5, 0x31, 0xf4, 0x05, 0x79);

static const ble_uuid128_t ANCS_NS_UUID = BLE_UUID128_INIT(
    0xbd, 0x1d, 0xa2, 0x99, 0xe6, 0x25, 0x58, 0x8c,
    0xd9, 0x42, 0x01, 0x63, 0x0d, 0x12, 0xbf, 0x9f);

static const ble_uuid128_t ANCS_CP_UUID = BLE_UUID128_INIT(
    0xd9, 0xd9, 0xaa, 0xfd, 0xbd, 0x9b, 0x21, 0x98,
    0xa8, 0x49, 0xe1, 0x45, 0xf3, 0xd8, 0xd1, 0x69);

static const ble_uuid128_t ANCS_DS_UUID = BLE_UUID128_INIT(
    0xfb, 0x7b, 0x7c, 0xce, 0x6a, 0xb3, 0x44, 0xbe,
    0xb5, 0x4b, 0xd6, 0x24, 0xe9, 0xc6, 0xea, 0x22);

static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static volatile bool s_connected = false;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

// Discovered ANCS handles
static uint16_t s_svc_start = 0, s_svc_end = 0;
static uint16_t s_ns_val_handle = 0, s_ns_cccd_handle = 0;
static uint16_t s_cp_val_handle = 0;
static uint16_t s_ds_val_handle = 0, s_ds_cccd_handle = 0;
static bool s_ancs_ready = false;

// Reassembly buffer for ANCS Data Source responses
static uint8_t s_ds_buf[384];
static size_t s_ds_len = 0;
static bool s_ds_expecting = false;

static ble_ancs_msg_t s_latest_msg = {0};
static SemaphoreHandle_t s_mutex = NULL;

void ble_store_config_init(void);
static void start_advertising(void);

static void store_msg(const char *app_id, const char *title, const char *message)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_latest_msg.seq++;
    strncpy(s_latest_msg.app_id, app_id ? app_id : "", sizeof(s_latest_msg.app_id) - 1);
    s_latest_msg.app_id[sizeof(s_latest_msg.app_id) - 1] = '\0';
    strncpy(s_latest_msg.title, title ? title : "", sizeof(s_latest_msg.title) - 1);
    s_latest_msg.title[sizeof(s_latest_msg.title) - 1] = '\0';
    strncpy(s_latest_msg.message, message ? message : "", sizeof(s_latest_msg.message) - 1);
    s_latest_msg.message[sizeof(s_latest_msg.message) - 1] = '\0';
    xSemaphoreGive(s_mutex);
    audio_play_beep_ok();
}

static void parse_data_source_buffer(void)
{
    // Format: [cmd_id:1][uid:4] followed by repeated [attr_id:1][len:2][val:len]
    if (s_ds_len < 5 || s_ds_buf[0] != 0) {
        return;
    }

    char app_id[48] = {0};
    char title[64] = {0};
    char message[192] = {0};
    bool got_app = false, got_title = false, got_msg = false;

    size_t pos = 5;
    while (pos + 3 <= s_ds_len) {
        uint8_t attr_id = s_ds_buf[pos];
        uint16_t alen = (uint16_t)(s_ds_buf[pos + 1] | (s_ds_buf[pos + 2] << 8));
        pos += 3;
        if (pos + alen > s_ds_len) {
            // Incomplete packet, wait for next BLE notification chunk
            return;
        }
        if (attr_id == 0) {
            size_t n = alen < sizeof(app_id) - 1 ? alen : sizeof(app_id) - 1;
            memcpy(app_id, &s_ds_buf[pos], n);
            app_id[n] = '\0';
            got_app = true;
        } else if (attr_id == 1) {
            size_t n = alen < sizeof(title) - 1 ? alen : sizeof(title) - 1;
            memcpy(title, &s_ds_buf[pos], n);
            title[n] = '\0';
            got_title = true;
        } else if (attr_id == 3) {
            size_t n = alen < sizeof(message) - 1 ? alen : sizeof(message) - 1;
            memcpy(message, &s_ds_buf[pos], n);
            message[n] = '\0';
            got_msg = true;
        }
        pos += alen;
    }

    if (got_app && got_title && got_msg) {
        s_ds_expecting = false;
        s_ds_len = 0;
        ESP_LOGI(TAG, "iPhone Notification [%s] %s: %s", app_id, title, message);

        // Filter for WeChat (com.tencent.xin)
        if (strstr(app_id, "tencent.xin") != NULL) {
            store_msg(app_id, title[0] ? title : "WeChat", message);
        }
    }
}

static void request_notification_attrs(uint32_t uid)
{
    if (!s_cp_val_handle || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    uint8_t cmd[12];
    cmd[0] = 0x00; // CommandIDGetNotificationAttributes
    cmd[1] = (uint8_t)(uid & 0xFF);
    cmd[2] = (uint8_t)((uid >> 8) & 0xFF);
    cmd[3] = (uint8_t)((uid >> 16) & 0xFF);
    cmd[4] = (uint8_t)((uid >> 24) & 0xFF);
    cmd[5] = 0x00; // Attr 0: AppIdentifier (no max len)
    cmd[6] = 0x01; // Attr 1: Title
    cmd[7] = 48;   // max len = 48
    cmd[8] = 0;
    cmd[9] = 0x03; // Attr 3: Message
    cmd[10] = 160; // max len = 160
    cmd[11] = 0;

    s_ds_len = 0;
    s_ds_expecting = true;
    ble_gattc_write_flat(s_conn_handle, s_cp_val_handle, cmd, sizeof(cmd), NULL, NULL);
}

static void subscribe_cccd(uint16_t cccd_handle)
{
    if (!cccd_handle || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    uint8_t val[2] = {0x01, 0x00}; // Enable Notifications
    ble_gattc_write_flat(s_conn_handle, cccd_handle, val, sizeof(val), NULL, NULL);
}

static int on_dsc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error->status == 0 && dsc) {
        if (ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
            if (chr_val_handle == s_ns_val_handle) {
                s_ns_cccd_handle = dsc->handle;
            } else if (chr_val_handle == s_ds_val_handle) {
                s_ds_cccd_handle = dsc->handle;
            }
        }
    } else if (error->status == BLE_HS_EDONE) {
        if (s_ds_cccd_handle && !s_ancs_ready) {
            subscribe_cccd(s_ds_cccd_handle);
        }
        if (s_ns_cccd_handle && !s_ancs_ready) {
            subscribe_cccd(s_ns_cccd_handle);
            s_ancs_ready = true;
            ESP_LOGI(TAG, "ANCS subscribed! Waiting for iPhone WeChat notifications...");
        }
    }
    return 0;
}

static int on_chr_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (error->status == 0 && chr) {
        if (ble_uuid_cmp(&chr->uuid.u, &ANCS_NS_UUID.u) == 0) {
            s_ns_val_handle = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &ANCS_CP_UUID.u) == 0) {
            s_cp_val_handle = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &ANCS_DS_UUID.u) == 0) {
            s_ds_val_handle = chr->val_handle;
        }
    } else if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "ANCS chars: NS=%u CP=%u DS=%u", s_ns_val_handle, s_cp_val_handle, s_ds_val_handle);
        if (s_ds_val_handle) {
            ble_gattc_disc_all_dscs(conn_handle, s_ds_val_handle, s_svc_end, on_dsc_disc, NULL);
        }
        if (s_ns_val_handle) {
            ble_gattc_disc_all_dscs(conn_handle, s_ns_val_handle, s_svc_end, on_dsc_disc, NULL);
        }
    }
    return 0;
}

static int on_svc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;
    if (error->status == 0 && service) {
        s_svc_start = service->start_handle;
        s_svc_end = service->end_handle;
    } else if (error->status == BLE_HS_EDONE) {
        if (s_svc_start != 0 && s_svc_end != 0) {
            ESP_LOGI(TAG, "ANCS service found (handles %u..%u)", s_svc_start, s_svc_end);
            ble_gattc_disc_all_chrs(conn_handle, s_svc_start, s_svc_end, on_chr_disc, NULL);
        } else {
            ESP_LOGW(TAG, "ANCS service not found on peer (ensure iPhone 'Share System Notifications' is ON)");
        }
    }
    return 0;
}

static void start_ancs_discovery(uint16_t conn_handle)
{
    s_svc_start = s_svc_end = 0;
    s_ns_val_handle = s_ns_cccd_handle = 0;
    s_cp_val_handle = 0;
    s_ds_val_handle = s_ds_cccd_handle = 0;
    s_ancs_ready = false;
    ble_gattc_disc_svc_by_uuid(conn_handle, &ANCS_SVC_UUID.u, on_svc_disc, NULL);
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_connected = true;
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE connected (handle=%u), initiating pairing...", s_conn_handle);
            ble_gap_security_initiate(s_conn_handle);
        } else {
            s_connected = false;
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected (reason=%d), restarting advertising", event->disconnect.reason);
        s_connected = false;
        s_ancs_ready = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "BLE link encrypted, discovering ANCS...");
            start_ancs_discovery(event->enc_change.conn_handle);
        } else {
            ESP_LOGW(TAG, "BLE encryption failed (status=%d)", event->enc_change.status);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint16_t attr_handle = event->notify_rx.attr_handle;
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (attr_handle == s_ns_val_handle && len >= 8) {
            uint8_t ns[8];
            ble_hs_mbuf_to_flat(event->notify_rx.om, ns, sizeof(ns), NULL);
            uint8_t event_id = ns[0];
            uint8_t event_flags = ns[1];
            uint32_t uid = (uint32_t)ns[4] | ((uint32_t)ns[5] << 8) |
                           ((uint32_t)ns[6] << 16) | ((uint32_t)ns[7] << 24);
            // EventIDNotificationAdded (0), ignore pre-existing (bit 2)
            if (event_id == 0 && (event_flags & 0x04) == 0) {
                request_notification_attrs(uid);
            }
        } else if (attr_handle == s_ds_val_handle && s_ds_expecting) {
            if (s_ds_len + len <= sizeof(s_ds_buf)) {
                ble_hs_mbuf_to_flat(event->notify_rx.om, s_ds_buf + s_ds_len, len, NULL);
                s_ds_len += len;
                parse_data_source_buffer();
            } else {
                s_ds_expecting = false;
                s_ds_len = 0;
            }
        }
        return 0;
    }

    default:
        return 0;
    }
}

static void start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)BLE_DEVICE_NAME;
    fields.name_len = strlen(BLE_DEVICE_NAME);
    fields.name_is_complete = 1;

    ble_gap_adv_set_fields(&fields);

    // Put ANCS Service Solicitation UUID in scan response so iOS recognizes ANCS support
    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = (ble_uuid128_t *)&ANCS_SVC_UUID;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // Use relaxed 200..300ms advertising interval so 2.4GHz Wi-Fi stays fast & responsive
    adv_params.itvl_min = 320;
    adv_params.itvl_max = 480;
    ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
}

static void nimble_on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    start_advertising();
    ESP_LOGI(TAG, "BLE advertising as '%s' (pair on iPhone & enable Share System Notifications)", BLE_DEVICE_NAME);
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_ancs_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = nimble_on_sync;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    ble_store_config_init();

    nimble_port_freertos_init(nimble_host_task);
    return ESP_OK;
}

bool ble_ancs_is_connected(void)
{
    return s_connected;
}

bool ble_ancs_get_latest_msg(ble_ancs_msg_t *out)
{
    if (!out || !s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_latest_msg;
    xSemaphoreGive(s_mutex);
    return out->seq > 0;
}

void ble_ancs_inject_test_msg(const char *title, const char *message)
{
    store_msg("com.tencent.xin", title, message);
}
