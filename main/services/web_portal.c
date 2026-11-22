#include "web_portal.h"
#include "app_config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
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

    char e_ssid[80], e_stt_url[192], e_stt_mod[96], e_llm_url[192], e_llm_mod[96];
    html_escape(cfg.wifi_ssid, e_ssid, sizeof(e_ssid));
    html_escape(cfg.ai_stt_url, e_stt_url, sizeof(e_stt_url));
    html_escape(cfg.ai_stt_model, e_stt_mod, sizeof(e_stt_mod));
    html_escape(cfg.ai_llm_url, e_llm_url, sizeof(e_llm_url));
    html_escape(cfg.ai_llm_model, e_llm_mod, sizeof(e_llm_mod));

    char *html = malloc(3200);
    if (!html) {
        return httpd_resp_send_500(req);
    }

    int len = snprintf(html, 3200,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32-C6 Clock Setup</title>"
        "<style>"
        "body{font-family:system-ui,sans-serif;background:#0d121e;color:#f5f8ff;margin:0;padding:18px;}"
        ".card{max-width:420px;margin:0 auto;background:#182134;border:1px solid #344462;border-radius:14px;padding:18px;}"
        "h2{margin:0 0 14px;font-size:18px;color:#38bdf8;}"
        "label{display:block;margin:10px 0 4px;font-size:13px;color:#94a3c0;}"
        "input{width:100%%;box-sizing:border-box;padding:9px 10px;border-radius:8px;border:1px solid #344462;background:#0d121e;color:#f5f8ff;font-size:14px;}"
        "button{margin-top:16px;width:100%%;padding:11px;border:0;border-radius:10px;background:#3b82f6;color:#fff;font-size:15px;font-weight:600;cursor:pointer;}"
        ".hint{font-size:12px;color:#94a3c0;margin-top:10px;line-height:1.5;}"
        "</style></head><body><div class='card'>"
        "<h2>ESP32-C6 Wi-Fi &amp; AI 配置</h2>"
        "<form method='POST' action='/save'>"
        "<label>Wi-Fi 名称 (SSID)</label><input name='ssid' value='%s' required>"
        "<label>Wi-Fi 密码 (留空保持不变)</label><input name='pass' type='password' placeholder='%s'>"
        "<label>AI API Key (留空保持不变)</label><input name='key' type='password' placeholder='%s'>"
        "<label>STT 接口 URL</label><input name='stt_url' value='%s'>"
        "<label>STT 模型</label><input name='stt_model' value='%s'>"
        "<label>LLM 接口 URL</label><input name='llm_url' value='%s'>"
        "<label>LLM 模型</label><input name='llm_model' value='%s'>"
        "<button type='submit'>保存并重启生效</button>"
        "</form>"
        "<div class='hint'>提示：密码与 API Key 已安全保存在设备 Flash (NVS) 中，无需重新烧录即可随时通过长按 BOOT 键 3 秒重新配置。</div>"
        "</div></body></html>",
        e_ssid,
        cfg.wifi_password[0] ? "已保存（留空则不修改）" : "输入 Wi-Fi 密码",
        cfg.ai_api_key[0] ? "已保存（留空则不修改）" : "输入 sk-... 密钥",
        e_stt_url, e_stt_mod, e_llm_url, e_llm_mod);

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
    if (form_get_field(body, "key", tmp, sizeof(cfg.ai_api_key)) && tmp[0] != '\0') {
        strncpy(cfg.ai_api_key, tmp, sizeof(cfg.ai_api_key) - 1);
        cfg.ai_api_key[sizeof(cfg.ai_api_key) - 1] = '\0';
    }
    if (form_get_field(body, "stt_url", tmp, sizeof(cfg.ai_stt_url)) && tmp[0] != '\0') {
        strncpy(cfg.ai_stt_url, tmp, sizeof(cfg.ai_stt_url) - 1);
        cfg.ai_stt_url[sizeof(cfg.ai_stt_url) - 1] = '\0';
    }
    if (form_get_field(body, "stt_model", tmp, sizeof(cfg.ai_stt_model)) && tmp[0] != '\0') {
        strncpy(cfg.ai_stt_model, tmp, sizeof(cfg.ai_stt_model) - 1);
        cfg.ai_stt_model[sizeof(cfg.ai_stt_model) - 1] = '\0';
    }
    if (form_get_field(body, "llm_url", tmp, sizeof(cfg.ai_llm_url)) && tmp[0] != '\0') {
        strncpy(cfg.ai_llm_url, tmp, sizeof(cfg.ai_llm_url) - 1);
        cfg.ai_llm_url[sizeof(cfg.ai_llm_url) - 1] = '\0';
    }
    if (form_get_field(body, "llm_model", tmp, sizeof(cfg.ai_llm_model)) && tmp[0] != '\0') {
        strncpy(cfg.ai_llm_model, tmp, sizeof(cfg.ai_llm_model) - 1);
        cfg.ai_llm_model[sizeof(cfg.ai_llm_model) - 1] = '\0';
    }

    free(body);
    app_config_save(&cfg);

    const char *ok_html =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:system-ui,sans-serif;background:#0d121e;color:#f5f8ff;text-align:center;padding:40px 20px;}"
        "h2{color:#22c55e;}</style></head><body>"
        "<h2>配置已保存！</h2><p>设备正在重启并连接新的 Wi-Fi...</p></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, ok_html, HTTPD_RESP_USE_STRLEN);

    xTaskCreate(delayed_restart_task, "portal_rst", 2048, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t web_portal_start(void)
{
    if (s_active) {
        return ESP_OK;
    }

    s_active = true;
    ESP_LOGI(TAG, "Starting SoftAP setup portal: SSID='%s' at http://192.168.4.1", WEB_PORTAL_AP_SSID);

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

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

bool web_portal_is_active(void)
{
    return s_active;
}

const char *web_portal_get_ap_ssid(void)
{
    return WEB_PORTAL_AP_SSID;
}
