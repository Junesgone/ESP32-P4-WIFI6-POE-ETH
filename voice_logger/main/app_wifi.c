/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#include "app_wifi.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          8

static EventGroupHandle_t s_events;
static int  s_retry;
static bool s_connected;

#define WIFI_NVS_NS "wifi"
static char s_ssid[33];
static char s_pass[65];

esp_err_t wifi_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "ssid", ssid ? ssid : "");
    if (err == ESP_OK) {
        err = nvs_set_str(h, "pass", password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "credentials saved to NVS: SSID '%s'", ssid ? ssid : "");
    }
    return err;
}

const char *wifi_current_ssid(void)
{
    return s_ssid;
}

/* NVS wins; the menuconfig values are only the factory default. */
static void load_credentials(void)
{
    snprintf(s_ssid, sizeof(s_ssid), "%s", CONFIG_ESP_WIFI_SSID);
    snprintf(s_pass, sizeof(s_pass), "%s", CONFIG_ESP_WIFI_PASSWORD);
    bool from_nvs = false;
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        char tmp[65];
        size_t len = sizeof(tmp);
        if (nvs_get_str(h, "ssid", tmp, &len) == ESP_OK && tmp[0]) {
            snprintf(s_ssid, sizeof(s_ssid), "%.32s", tmp);
            from_nvs = true;
        }
        len = sizeof(tmp);
        if (nvs_get_str(h, "pass", tmp, &len) == ESP_OK) {
            snprintf(s_pass, sizeof(s_pass), "%s", tmp);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "credentials: SSID '%s' (from %s)", s_ssid,
             from_nvs ? "NVS" : "menuconfig");
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry < MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "disconnected, retry %d/%d", s_retry, MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip:" IPSTR ", gw:" IPSTR, IP2STR(&evt->ip_info.ip), IP2STR(&evt->ip_info.gw));
        s_retry = 0;
        s_connected = true;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_connect(uint32_t timeout_ms)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init");
    load_credentials();

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_sta();

    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "event group");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            &on_event, NULL, NULL),
                        TAG, "wifi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            &on_event, NULL, NULL),
                        TAG, "ip event handler");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init");

    wifi_config_t wc = { 0 };
    /* ssid[] is 32 bytes and password[] 64, so leave one byte for the NUL. */
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%.31s", s_ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%.63s", s_pass);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wc), TAG, "set_config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");

    ESP_LOGI(TAG, "connecting to SSID '%s' ...", s_ssid);
    EventBits_t bits = xEventGroupWaitBits(s_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "connection failed after %d retries", MAX_RETRY);
    } else {
        ESP_LOGW(TAG, "no Wi-Fi within %u ms (radio 2.4 GHz only; antenna attached?)",
                 (unsigned)timeout_ms);
    }
    return ESP_FAIL;
}

int wifi_scan(char ssids[][33], int8_t *rssi, int max)
{
    wifi_scan_config_t cfg = { 0 };
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) {
        return -1;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        return 0;
    }
    if (n > 32) {
        n = 32;
    }
    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) {
        return -1;
    }
    int got = (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) ? (int)n : 0;

    /* Strongest first so the list is useful even when truncated. */
    for (int i = 0; i < got; i++) {
        for (int j = i + 1; j < got; j++) {
            if (recs[j].rssi > recs[i].rssi) {
                wifi_ap_record_t t = recs[i];
                recs[i] = recs[j];
                recs[j] = t;
            }
        }
    }
    int out = got < max ? got : max;
    for (int i = 0; i < out; i++) {
        snprintf(ssids[i], 33, "%.32s", (const char *)recs[i].ssid);
        rssi[i] = recs[i].rssi;
    }
    free(recs);
    ESP_LOGI(TAG, "scan found %d AP(s)", got);
    return out;
}

bool wifi_link_info(char *ssid_out, size_t ssid_len, char *ip_out, size_t ip_len, int *rssi_out)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        if (ssid_out) {
            snprintf(ssid_out, ssid_len, "%.32s", (const char *)ap.ssid);
        }
        if (rssi_out) {
            *rssi_out = ap.rssi;
        }
    } else {
        if (ssid_out) {
            ssid_out[0] = 0;
        }
        if (rssi_out) {
            *rssi_out = 0;
        }
    }
    if (ip_out) {
        esp_netif_ip_info_t ip;
        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr) {
            snprintf(ip_out, ip_len, IPSTR, IP2STR(&ip.ip));
        } else {
            snprintf(ip_out, ip_len, "none");
        }
    }
    return wifi_connected();
}

esp_err_t wifi_join(const char *ssid, const char *password, uint32_t timeout_ms)
{
    wifi_config_t cfg = { 0 };
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", ssid);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", password ? password : "");
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_LOGI(TAG, "joining '%s' ...", ssid);
    /* Persist first: a voice/console "connect to X" must survive a reboot. */
    if (wifi_save_credentials(ssid, password ? password : "") == ESP_OK) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
        snprintf(s_pass, sizeof(s_pass), "%s", password ? password : "");
    }
    if (esp_wifi_disconnect() != ESP_OK) {
        /* not fatal: it may simply not have been connected */
    }
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        return ESP_FAIL;
    }
    return wifi_connect(timeout_ms);
}

bool wifi_connected(void)
{
    return s_connected;
}

esp_err_t wifi_time_sync(uint32_t timeout_ms)
{
    /* The board is used as a fixed conversation logger in CST; records are
     * written with the local offset so they read correctly on a PC. */
    setenv("TZ", "CST-8", 1);
    tzset();

    /* ESP-IDF defaults to pool.ntp.org, which times out from mainland China
     * (measured: no reply in 3 s, while the servers below answer in 20-80 ms).
     * That made time sync + conv/<date>/ folders work only intermittently.
     * A second server is configured as a fallback. */
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
        ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "cn.pool.ntp.org"));
    sntp.start = true;
    esp_err_t err = esp_netif_sntp_init(&sntp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sntp init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sntp sync timeout, falling back to uptime stamps");
    } else {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char buf[40];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S%z", &tm);
        ESP_LOGI(TAG, "time synced: %s", buf);
    }
    esp_netif_sntp_deinit();
    return err;
}

/* ------------------------- hotspot provisioning -------------------------- */

static httpd_handle_t s_httpd;
static bool s_ap_netif_created;
static volatile bool s_got_creds;
static char s_new_ssid[33];
static char s_new_pass[65];

static void url_decode(char *dst, size_t dst_len, const char *src, size_t src_len)
{
    size_t o = 0;
    for (size_t i = 0; i < src_len && o + 1 < dst_len; i++) {
        char ch = src[i];
        if (ch == '+') {
            dst[o++] = ' ';
        } else if (ch == '%' && i + 2 < src_len) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[o++] = ch;
        }
    }
    dst[o] = 0;
}

static void form_field(const char *body, const char *key, char *out, size_t out_len)
{
    out[0] = 0;
    size_t klen = strlen(key);
    const char *p = body;
    while ((p = strstr(p, key)) != NULL) {
        if (p > body && p[-1] != '&') {
            p += klen;
            continue;
        }
        p += klen;
        if (*p != '=') {
            continue;
        }
        p++;
        const char *end = strchr(p, '&');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        url_decode(out, out_len, p, n);
        return;
    }
}

static esp_err_t http_root(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>小新配网</title></head>"
        "<body style='font-family:sans-serif;max-width:420px;margin:24px auto'>"
        "<h2>小新 WiFi 配网</h2>"
        "<form method='post' action='/save'>"
        "<p>WiFi 名称<br><input name='ssid' required style='width:100%;padding:8px'></p>"
        "<p>密码<br><input name='password' style='width:100%;padding:8px'></p>"
        "<p><button type='submit' style='padding:10px 18px'>连接</button></p>"
        "</form></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_save(httpd_req_t *req)
{
    char body[512];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }
    body[len] = 0;
    form_field(body, "ssid", s_new_ssid, sizeof(s_new_ssid));
    form_field(body, "password", s_new_pass, sizeof(s_new_pass));
    if (!s_new_ssid[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
    }
    s_got_creds = true;
    static const char ok[] =
        "<!doctype html><html><head><meta charset='utf-8'></head>"
        "<body style='font-family:sans-serif;margin:24px'>"
        "<h2>已收到，正在连接…</h2><p>板子会自动切回 STA 模式，串口会打印 IP。</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, ok, HTTPD_RESP_USE_STRLEN);
}

esp_err_t wifi_start_provisioning(uint32_t timeout_ms)
{
    if (!s_ap_netif_created) {
        ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_ap() != NULL, ESP_FAIL,
                            TAG, "create AP netif");
        s_ap_netif_created = true;
    }

    char ap_ssid[33];
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(ap_ssid, sizeof(ap_ssid), "Xiaozhi-%02X%02X", mac[4], mac[5]);

    wifi_config_t apc = { 0 };
    snprintf((char *)apc.ap.ssid, sizeof(apc.ap.ssid), "%.31s", ap_ssid);
    apc.ap.ssid_len = (uint8_t)strlen(ap_ssid);
    apc.ap.channel = 1;
    apc.ap.max_connection = 4;
    apc.ap.authmode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set APSTA");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &apc), TAG, "AP config");

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.lru_purge_enable = true;
    hc.stack_size = 6144;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &hc), TAG, "httpd start");

    httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = http_root, .user_ctx = NULL };
    httpd_uri_t uri_save = { .uri = "/save", .method = HTTP_POST, .handler = http_save, .user_ctx = NULL };
    httpd_uri_t uri_204 = { .uri = "/generate_204", .method = HTTP_GET, .handler = http_root, .user_ctx = NULL };
    httpd_register_uri_handler(s_httpd, &uri_root);
    httpd_register_uri_handler(s_httpd, &uri_save);
    httpd_register_uri_handler(s_httpd, &uri_204);

    s_got_creds = false;
    ESP_LOGW(TAG, "==== 配网模式 ====");
    ESP_LOGW(TAG, "手机连接热点 '%s'（无密码），浏览器打开 http://192.168.4.1/", ap_ssid);

    int64_t deadline = esp_timer_get_time() +
                       (int64_t)(timeout_ms ? timeout_ms : 180000) * 1000;
    while (!s_got_creds && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    httpd_stop(s_httpd);
    s_httpd = NULL;
    esp_wifi_set_mode(WIFI_MODE_STA);

    if (!s_got_creds) {
        ESP_LOGW(TAG, "配网超时，没有收到凭据");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "收到凭据: SSID '%s'", s_new_ssid);
    wifi_save_credentials(s_new_ssid, s_new_pass);
    snprintf(s_ssid, sizeof(s_ssid), "%s", s_new_ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", s_new_pass);
    s_connected = false;
    s_retry = 0;
    return wifi_connect(25000);
}
