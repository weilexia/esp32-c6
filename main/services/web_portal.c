#include "web_portal.h"
#include "alert_server.h"
#include "app_config.h"
#include "wifi_time.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "web_portal";
static volatile bool s_active = false;
static httpd_handle_t s_server = NULL;
static esp_netif_t *s_ap_netif = NULL;

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *src, size_t src_len, char *dst, size_t dst_sz)
{
    size_t i = 0, j = 0;
    while (i < src_len && j + 1 < dst_sz) {
        if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else if (src[i] == '%' && i + 2 < src_len) {
            int hi = hex_val(src[i + 1]);
            int lo = hex_val(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[j++] = (char)((hi << 4) | lo);
                i += 3;
            } else {
                dst[j++] = src[i++];
            }
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

static bool form_get_field(const char *body, const char *key, char *out, size_t out_sz)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seg_len = amp ? (size_t)(amp - p) : strlen(p);
        if (seg_len > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            url_decode(p + klen + 1, seg_len - klen - 1, out, out_sz);
            return true;
        }
        p = amp ? (amp + 1) : NULL;
    }
    return false;
}

static void html_escape(const char *src, char *dst, size_t dst_sz)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 6 < dst_sz; i++) {
        char c = src[i];
        if (c == '"') { memcpy(dst + j, "&quot;", 6); j += 6; }
        else if (c == '&') { memcpy(dst + j, "&amp;", 5); j += 5; }
        else if (c == '<') { memcpy(dst + j, "&lt;", 4); j += 4; }
        else if (c == '>') { memcpy(dst + j, "&gt;", 4); j += 4; }
        else { dst[j++] = c; }
    }
    dst[j] = '\0';
}

static void delayed_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t handle_get_root(httpd_req_t *req)
{
    app_config_t cfg;
    app_config_get(&cfg);

    char e_ssid[80], e_budget[32];
    html_escape(cfg.wifi_ssid, e_ssid, sizeof(e_ssid));
    html_escape(cfg.deepseek_daily_budget[0] ? cfg.deepseek_daily_budget : "20.0", e_budget, sizeof(e_budget));

    char *html = malloc(3200);
    if (!html) {
        return httpd_resp_send_500(req);
    }

    int len = snprintf(html, 3200,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>DeepSeek Usage Deck Setup</title>"
        "<style>"
        "body{font-family:system-ui,-apple-system,sans-serif;background:#05080d;color:#f1f5f9;margin:0;padding:18px;}"
        ".card{max-width:420px;margin:0 auto;background:#0f1622;border:1px solid #1e2d40;border-radius:16px;padding:20px;}"
        ".hdr{display:flex;align-items:center;gap:10px;margin-bottom:16px;}"
        ".badge{width:34px;height:34px;border-radius:9px;background:#4ade80;color:#05080d;font-weight:800;display:flex;align-items:center;justify-content:center;font-size:15px;}"
        "h2{margin:0;font-size:17px;color:#4ade80;letter-spacing:.5px;}"
        ".sub{font-size:12px;color:#7d94b0;margin-top:2px;}"
        "label{display:block;margin:12px 0 5px;font-size:13px;color:#94a3b8;}"
        "input{width:100%%;box-sizing:border-box;padding:10px 12px;border-radius:10px;border:1px solid #26364c;background:#080d14;color:#f8fafc;font-size:14px;outline:none;}"
        "input:focus{border-color:#4ade80;}"
        "button{margin-top:18px;width:100%%;padding:12px;border:0;border-radius:10px;background:#4ade80;color:#05080d;font-size:15px;font-weight:700;cursor:pointer;}"
        ".hint{font-size:12px;color:#7d94b0;margin-top:12px;line-height:1.6;}"
        "</style></head><body><div class='card'>"
        "<div class='hdr'><div class='badge'>DS</div><div><h2>DEEPSEEK USAGE DECK</h2><div class='sub'>Wi-Fi &amp; Token 监控配网 (192.168.1.4)</div></div></div>"
        "<form method='POST' action='/save'>"
        "<label>Wi-Fi 名称 (SSID)</label>"
        "<input name='ssid' value='%s' placeholder='输入 2.4GHz Wi-Fi 名称' required>"
        "<label>Wi-Fi 密码 (留空保持不变)</label>"
        "<input name='pass' type='password' placeholder='%s'>"
        "<label>DeepSeek API Key (用于监控余额与消耗)</label>"
        "<input name='ds_key' type='password' placeholder='%s'>"
        "<label>每日预算 (RMB)</label>"
        "<input name='ds_budget' value='%s' placeholder='20.0'>"
        "<button type='submit'>保存并重启连接 Wi-Fi</button>"
        "</form>"
        "<div class='hint'>提示：配置将安全保存在设备 Flash (NVS) 中。点击屏幕右下角「设置」按钮即可随时重新开启热点访问 192.168.1.4。</div>"
        "</div></body></html>",
        e_ssid,
        cfg.wifi_password[0] ? "已保存（留空则不修改）" : "输入 Wi-Fi 密码",
        cfg.deepseek_api_key[0] ? "已保存（留空则不修改）" : "输入 sk-... DeepSeek API Key",
        e_budget);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, html, len);
    free(html);
    return err;
}

static esp_err_t handle_post_save(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 1536) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body length");
    }

    char *body = malloc((size_t)total_len + 1);
    if (!body) {
        return httpd_resp_send_500(req);
    }

    int received = 0;
    while (received < total_len) {
        int r = httpd_req_recv(req, body + received, total_len - received);
        if (r <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    app_config_t cfg;
    app_config_get(&cfg);

    char tmp[128];
    if (form_get_field(body, "ssid", tmp, sizeof(cfg.wifi_ssid))) {
        strncpy(cfg.wifi_ssid, tmp, sizeof(cfg.wifi_ssid) - 1);
        cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1] = '\0';
    }
    if (form_get_field(body, "pass", tmp, sizeof(cfg.wifi_password)) && tmp[0] != '\0') {
        strncpy(cfg.wifi_password, tmp, sizeof(cfg.wifi_password) - 1);
        cfg.wifi_password[sizeof(cfg.wifi_password) - 1] = '\0';
    }
    if (form_get_field(body, "ds_key", tmp, sizeof(cfg.deepseek_api_key)) && tmp[0] != '\0') {
        strncpy(cfg.deepseek_api_key, tmp, sizeof(cfg.deepseek_api_key) - 1);
        cfg.deepseek_api_key[sizeof(cfg.deepseek_api_key) - 1] = '\0';
    }
    if (form_get_field(body, "ds_budget", tmp, sizeof(cfg.deepseek_daily_budget)) && tmp[0] != '\0') {
        strncpy(cfg.deepseek_daily_budget, tmp, sizeof(cfg.deepseek_daily_budget) - 1);
        cfg.deepseek_daily_budget[sizeof(cfg.deepseek_daily_budget) - 1] = '\0';
    }

    free(body);
    app_config_save(&cfg);

    const char *ok_html =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:system-ui,sans-serif;background:#05080d;color:#f1f5f9;text-align:center;padding:48px 20px;}"
        "h2{color:#4ade80;}p{color:#94a3b8;}</style></head><body>"
        "<h2>配置已保存！</h2><p>设备正在重启并连接新的 Wi-Fi...</p></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, ok_html, HTTPD_RESP_USE_STRLEN);

    xTaskCreate(delayed_restart_task, "portal_rst", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static void configure_ap_static_ip(esp_netif_t *ap_netif)
{
    esp_netif_dhcps_stop(ap_netif);

    esp_netif_ip_info_t ip_info = {0};
    IP4_ADDR(&ip_info.ip, 192, 168, 1, 4);
    IP4_ADDR(&ip_info.gw, 192, 168, 1, 4);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    esp_err_t err = esp_netif_set_ip_info(ap_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_set_ip_info(192.168.1.4) failed: %s", esp_err_to_name(err));
    }

    esp_netif_dhcps_start(ap_netif);
    ESP_LOGI(TAG, "SoftAP static IP configured to http://%s", WEB_PORTAL_AP_IP);
}

esp_err_t web_portal_start(void)
{
    if (s_active) {
        return ESP_OK;
    }

    // Release port 80 if the STA alert/usage HTTP server is currently running
    alert_server_stop_httpd();

    s_active = true;
    ESP_LOGI(TAG, "Starting SoftAP setup portal: SSID='%s' at http://%s",
             WEB_PORTAL_AP_SSID, WEB_PORTAL_AP_IP);

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    configure_ap_static_ip(s_ap_netif);

    esp_wifi_stop();

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, WEB_PORTAL_AP_SSID, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(WEB_PORTAL_AP_SSID);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (!s_server) {
        httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
        hcfg.stack_size = 6144;
        esp_err_t err = httpd_start(&s_server, &hcfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
            return err;
        }

        const httpd_uri_t uri_root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = handle_get_root,
        };
        const httpd_uri_t uri_save = {
            .uri = "/save",
            .method = HTTP_POST,
            .handler = handle_post_save,
        };
        httpd_register_uri_handler(s_server, &uri_root);
        httpd_register_uri_handler(s_server, &uri_save);
    }

    return ESP_OK;
}

esp_err_t web_portal_stop(void)
{
    if (!s_active) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping SoftAP portal and resuming Wi-Fi STA...");
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    s_active = false;
    wifi_time_reconnect_sta();
    return ESP_OK;
}

bool web_portal_is_active(void)
{
    return s_active;
}

const char *web_portal_get_ap_ssid(void)
{
    return WEB_PORTAL_AP_SSID;
}

const char *web_portal_get_ap_ip(void)
{
    return WEB_PORTAL_AP_IP;
}
