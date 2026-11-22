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

#define BLE_DEVICE_NAME "ESP32-UsageDeck"

// Minimal Consumer Control HID Report Map (Usage Page: Consumer, Usage: Consumer Control)
// Using Consumer Control (instead of Keyboard) ensures iOS shows the device in Settings -> Bluetooth
// WITHOUT disabling the iPhone on-screen virtual keyboard!
static const uint8_t s_hid_report_map[] = {
    0x05, 0x0C,       // Usage Page (Consumer)
    0x09, 0x01,       // Usage (Consumer Control)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x01,       //   Report ID (1)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x08,       //   Report Count (8)
    0x09, 0xE9,       //   Usage (Volume Increment)
    0x09, 0xEA,       //   Usage (Volume Decrement)
    0x09, 0xE2,       //   Usage (Mute)
    0x09, 0xCD,       //   Usage (Play/Pause)
    0x09, 0xB5,       //   Usage (Scan Next Track)
    0x09, 0xB6,       //   Usage (Scan Previous Track)
    0x09, 0xB7,       //   Usage (Stop)
    0x09, 0xB8,       //   Usage (Eject)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0xC0              // End Collection
};

static const uint8_t s_hid_info[4] = {0x11, 0x01, 0x00, 0x02}; // HID v1.11, NormallyConnectable
static const uint8_t s_hid_report_ref[2] = {0x01, 0x01};       // Report ID 1, Input
static uint16_t s_hid_report_handle = 0;

static int hid_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    uint16_t uuid16 = ble_uuid_u16(ctxt->chr ? ctxt->chr->uuid : ctxt->dsc->uuid);
    (void)arg;

    if (uuid16 == 0x2A4A) { // HID Information
        os_mbuf_append(ctxt->om, s_hid_info, sizeof(s_hid_info));
        return 0;
    } else if (uuid16 == 0x2A4B) { // Report Map
        os_mbuf_append(ctxt->om, s_hid_report_map, sizeof(s_hid_report_map));
        return 0;
    } else if (uuid16 == 0x2A4D) { // Report
        uint8_t zero = 0;
        os_mbuf_append(ctxt->om, &zero, 1);
        return 0;
    } else if (uuid16 == 0x2908) { // Report Reference Descriptor
        os_mbuf_append(ctxt->om, s_hid_report_ref, sizeof(s_hid_report_ref));
        return 0;
    } else if (uuid16 == 0x2A4C) { // HID Control Point
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_hid_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812), // Human Interface Device Service
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4A), // HID Info
                .access_cb = hid_access_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4B), // Report Map
                .access_cb = hid_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4C), // Control Point
                .access_cb = hid_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A4D), // Input Report
                .access_cb = hid_access_cb,
                .val_handle = &s_hid_report_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid = BLE_UUID16_DECLARE(0x2908), // Report Reference
                        .att_flags = BLE_ATT_F_READ,
                        .access_cb = hid_access_cb,
                    },
                    {0}
                },
            },
            {0}
        },
    },
    {0}
};

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

static void utf8_trim_incomplete(char *s)
{
    size_t len = strlen(s);
    while (len > 0) {
        uint8_t c = (uint8_t)s[len - 1];
        if (c < 0x80) break; // Valid 1-byte ASCII
        // Walk back to find UTF-8 lead byte
        size_t i = len - 1;
        while (i > 0 && (((uint8_t)s[i] & 0xC0) == 0x80)) {
            i--;
        }
        uint8_t lead = (uint8_t)s[i];
        size_t expected = 1;
        if ((lead & 0xE0) == 0xC0) expected = 2;
        else if ((lead & 0xF0) == 0xE0) expected = 3;
        else if ((lead & 0xF8) == 0xF0) expected = 4;
        else { s[i] = '\0'; len = i; continue; }

        if (len - i < expected) {
            s[i] = '\0';
            len = i;
        } else {
            break;
        }
    }
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
            utf8_trim_incomplete(title);
            got_title = true;
        } else if (attr_id == 3) {
            size_t n = alen < sizeof(message) - 1 ? alen : sizeof(message) - 1;
            memcpy(message, &s_ds_buf[pos], n);
            message[n] = '\0';
            utf8_trim_incomplete(message);
            got_msg = true;
        }
        pos += alen;
    }

    if (got_app && got_title && got_msg) {
        s_ds_expecting = false;
        s_ds_len = 0;
        ESP_LOGI(TAG, "iPhone Notification [%s] %s: %s", app_id, title, message);

        // Filter for WeChat (com.tencent.xin) & WeCom (com.tencent.ww)
        if (strstr(app_id, "tencent.xin") != NULL || strstr(app_id, "tencent.ww") != NULL) {
            store_msg(app_id, title[0] ? title : "微信", message[0] ? message : "收到一条新消息");
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

static int on_cccd_write(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    const char *label = (const char *)arg;
    ESP_LOGI(TAG, "ANCS %s CCCD (handle=%u) subscribe status=%d",
             label ? label : "?", attr ? attr->handle : 0, error->status);
    return 0;
}

static void subscribe_cccd(uint16_t conn_handle, uint16_t cccd_handle, const char *label)
{
    if (!cccd_handle || conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    uint8_t val[2] = {0x01, 0x00}; // Enable Notifications
    int rc = ble_gattc_write_flat(conn_handle, cccd_handle, val, sizeof(val), on_cccd_write, (void *)label);
    if (rc != 0) {
        ESP_LOGE(TAG, "subscribe_cccd(%s, handle=%u) failed rc=%d", label, cccd_handle, rc);
    }
}

// Step 2: Discover Notification Source (NS) CCCD (take ONLY the FIRST 0x2902 so we never walk into DS!)
static int on_ns_dsc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                          uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle;
    (void)arg;
    if (error->status == 0 && dsc) {
        if (s_ns_cccd_handle == 0 && ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
            s_ns_cccd_handle = dsc->handle;
        }
    } else if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "ANCS CCCDs found: NS_CCCD=%u DS_CCCD=%u", s_ns_cccd_handle, s_ds_cccd_handle);
        if (s_ds_cccd_handle) {
            subscribe_cccd(conn_handle, s_ds_cccd_handle, "DS");
        }
        if (s_ns_cccd_handle) {
            subscribe_cccd(conn_handle, s_ns_cccd_handle, "NS");
            s_ancs_ready = true;
            ESP_LOGI(TAG, "ANCS subscribed! Waiting for iPhone WeChat notifications...");
        } else {
            ESP_LOGW(TAG, "ANCS NS CCCD not found!");
        }
    }
    return 0;
}

// Step 1: Discover Data Source (DS) CCCD first, then sequentially discover NS CCCD
static int on_ds_dsc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                          uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle;
    (void)arg;
    if (error->status == 0 && dsc) {
        if (s_ds_cccd_handle == 0 && ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
            s_ds_cccd_handle = dsc->handle;
        }
    } else if (error->status == BLE_HS_EDONE) {
        if (s_ns_val_handle) {
            ble_gattc_disc_all_dscs(conn_handle, s_ns_val_handle, s_svc_end, on_ns_dsc_disc, NULL);
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
            ble_gattc_disc_all_dscs(conn_handle, s_ds_val_handle, s_svc_end, on_ds_dsc_disc, NULL);
        } else if (s_ns_val_handle) {
            ble_gattc_disc_all_dscs(conn_handle, s_ns_val_handle, s_svc_end, on_ns_dsc_disc, NULL);
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
            ESP_LOGI(TAG, "BLE link encrypted, exchanging MTU & discovering ANCS...");
            ble_gattc_exchange_mtu(event->enc_change.conn_handle, NULL, NULL);
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
    // Primary ADV packet (28 bytes <= 31): Flags (3) + HID UUID 0x1812 (4) + Appearance (4) + Name (17)
    // Advertising HID 0x1812 in primary ADV makes iPhone Settings -> Bluetooth display the device natively!
    static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids16 = (ble_uuid16_t *)&hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    fields.appearance = 0x03C0; // Generic HID (recognized natively by iOS & Android Bluetooth Settings)
    fields.appearance_is_present = 1;
    fields.name = (const uint8_t *)BLE_DEVICE_NAME;
    fields.name_len = strlen(BLE_DEVICE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: rc=%d", rc);
        return;
    }

    // Scan Response packet (18 bytes <= 31): Apple ANCS 128-bit Service Solicitation UUID (AD Type 0x15)
    uint8_t rsp_raw[18];
    rsp_raw[0] = 17;   // length: 1 byte AD type + 16 bytes UUID
    rsp_raw[1] = 0x15; // AD Type: List of 128-bit Service Solicitation UUIDs (required by Apple ANCS spec)
    memcpy(&rsp_raw[2], ANCS_SVC_UUID.value, 16);

    rc = ble_gap_adv_rsp_set_data(rsp_raw, sizeof(rsp_raw));
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_data failed: rc=%d", rc);
    }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // 100..150ms interval: fast phone discovery while keeping 2.4GHz Wi-Fi coexistence smooth
    adv_params.itvl_min = 160;
    adv_params.itvl_max = 240;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "BLE advertising started: name='%s' (HID 0x1812 + ANCS 0x15)", BLE_DEVICE_NAME);
    }
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
    ble_gatts_count_cfg(s_hid_gatt_svcs);
    ble_gatts_add_svcs(s_hid_gatt_svcs);
    ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    ble_store_config_init();

    nimble_port_freertos_init(nimble_host_task);
    return ESP_OK;
}

bool ble_ancs_is_connected(void)
{
    return s_connected;
}

bool ble_ancs_is_ready(void)
{
    return s_connected && s_ancs_ready;
}

const char *ble_ancs_get_device_name(void)
{
    return BLE_DEVICE_NAME;
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
