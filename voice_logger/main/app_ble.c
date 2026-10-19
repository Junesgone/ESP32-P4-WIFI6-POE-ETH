/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#include "app_ble.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "ble";

#define BLE_MAX_SEEN 24

typedef struct {
    char    name[32];
    uint8_t addr[6];
    int8_t  rssi;
} seen_t;

static uint8_t s_own_addr_type;
static bool    s_synced;
static bool    s_scanning;
static seen_t s_seen[BLE_MAX_SEEN];
static int     s_seen_n;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason %d", reason);
    s_synced = false;
}

static void on_sync(void)
{
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "cannot determine own address type");
        return;
    }
    s_synced = true;
    ESP_LOGI(TAG, "NimBLE host synced (addr type %d)", s_own_addr_type);
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void remember(const char *name, const uint8_t *addr, int8_t rssi)
{
    for (int i = 0; i < s_seen_n; i++) {
        if (memcmp(s_seen[i].addr, addr, 6) == 0) {
            if (rssi > s_seen[i].rssi) {
                s_seen[i].rssi = rssi;
            }
            if (name && !s_seen[i].name[0]) {
                snprintf(s_seen[i].name, sizeof(s_seen[i].name), "%s", name);
            }
            return;
        }
    }
    if (s_seen_n >= BLE_MAX_SEEN) {
        return;
    }
    seen_t *e = &s_seen[s_seen_n++];
    memset(e, 0, sizeof(*e));
    if (name) {
        snprintf(e->name, sizeof(e->name), "%s", name);
    }
    memcpy(e->addr, addr, 6);
    e->rssi = rssi;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        memset(&fields, 0, sizeof(fields));
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) == 0) {
            char nm[33] = { 0 };
            if (fields.name && fields.name_len) {
                int n = fields.name_len < 32 ? fields.name_len : 32;
                memcpy(nm, fields.name, (size_t)n);
                nm[n] = 0;
            }
            remember(nm[0] ? nm : NULL, event->disc.addr.val, event->disc.rssi);
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected (handle %d)", s_conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed, status %d", event->connect.status);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected, reason %d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        return 0;
    default:
        return 0;
    }
}

esp_err_t ble_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);

    /* Wait for the host to sync with the C6's controller. */
    for (int i = 0; i < 100 && !s_synced; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_synced) {
        /* The C6 is not answering HCI (its flashed firmware has no BT
         * controller running).  NimBLE would otherwise retry the reset every
         * two seconds forever and flood the console, so shut it down and let
         * the rest of the assistant carry on.  The ble_* tools report the
         * failure honestly instead of hanging. */
        ESP_LOGW(TAG, "NimBLE did not sync - the C6 BT controller is not responding;"
                      " reflash the slave with BT enabled. Disabling BLE.");
        nimble_port_stop();
        nimble_port_deinit();
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool ble_ready(void)
{
    return s_synced;
}

int ble_scan(char names[][32], uint8_t addrs[][6], int8_t *rssi, int max,
             uint32_t duration_ms)
{
    if (!s_synced) {
        return -1;
    }
    s_seen_n = 0;
    s_scanning = true;

    struct ble_gap_disc_params params;
    memset(&params, 0, sizeof(params));
    params.passive = 0;          /* active scan: also asks for scan responses */
    params.filter_duplicates = 1;

    int rc = ble_gap_disc(s_own_addr_type, (int32_t)duration_ms, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
        s_scanning = false;
        return -1;
    }

    uint32_t waited = 0;
    while (s_scanning && waited < duration_ms + 3000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    if (s_scanning) {
        ble_gap_disc_cancel();
        s_scanning = false;
    }

    /* Strongest first. */
    for (int i = 0; i < s_seen_n; i++) {
        for (int j = i + 1; j < s_seen_n; j++) {
            if (s_seen[j].rssi > s_seen[i].rssi) {
                seen_t t = s_seen[i];
                s_seen[i] = s_seen[j];
                s_seen[j] = t;
            }
        }
    }
    int n = s_seen_n < max ? s_seen_n : max;
    for (int i = 0; i < n; i++) {
        /* Explicit precision: gcc cannot prove the two 32-byte arrays match
         * and -Werror=format-truncation turns that into a build failure. */
        snprintf(names[i], 32, "%.*s", 31, s_seen[i].name);
        memcpy(addrs[i], s_seen[i].addr, 6);
        rssi[i] = s_seen[i].rssi;
    }
    ESP_LOGI(TAG, "scan found %d device(s) in %u ms", s_seen_n, (unsigned)duration_ms);
    return n;
}

esp_err_t ble_connect(const uint8_t addr[6])
{
    if (!s_synced) {
        return ESP_ERR_INVALID_STATE;
    }
    ble_addr_t peer;
    peer.type = BLE_ADDR_PUBLIC;
    memcpy(peer.val, addr, 6);
    int rc = ble_gap_connect(s_own_addr_type, &peer, 10000, NULL, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_connect rc=%d", rc);
        return ESP_FAIL;
    }
    for (int i = 0; i < 120 && s_conn_handle == BLE_HS_CONN_HANDLE_NONE; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_disconnect(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    return ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM) == 0 ? ESP_OK : ESP_FAIL;
}

bool ble_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}
