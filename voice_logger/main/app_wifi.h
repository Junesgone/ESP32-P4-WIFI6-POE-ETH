#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Connects to the 2.4 GHz AP configured in menuconfig.  Wi-Fi is provided by the
 * on-board ESP32-C6 through ESP-Hosted (SDIO), so this blocks while the link
 * comes up.  Returns ESP_OK once an IP address has been obtained. */
esp_err_t wifi_connect(uint32_t timeout_ms);

bool wifi_connected(void);

/* Scans for 2.4 GHz APs.  Fills up to max SSIDs, returns how many were found
 * (strongest first).  Blocks for a few seconds. */
int wifi_scan(char ssids[][33], int8_t *rssi, int max);

/* Current link details; returns false when not associated. */
bool wifi_link_info(char *ssid_out, size_t ssid_len, char *ip_out, size_t ip_len, int *rssi_out);

/* Joins a specific AP and PERSISTS the credentials to NVS, so a reboot stays on
 * the new network instead of falling back to the menuconfig one. */
esp_err_t wifi_join(const char *ssid, const char *password, uint32_t timeout_ms);

/* SSID currently in use (NVS override, else the menuconfig default). */
const char *wifi_current_ssid(void);

/* Write credentials to NVS.  They are picked up by the next wifi_connect(). */
esp_err_t wifi_save_credentials(const char *ssid, const char *password);

/* Hotspot provisioning: raises an open AP "Xiaozhi-XXXX" serving a small form at
 * http://192.168.4.1/ .  Typing an SSID/password on a phone is the only reliable
 * way to enter a WPA key - speech recognition cannot reproduce random
 * mixed-case/symbol passwords.  On submit (or timeout) the AP is torn down, the
 * credentials are saved to NVS and the board joins the new network.
 * Returns ESP_OK after connecting. */
esp_err_t wifi_start_provisioning(uint32_t timeout_ms);

/* SNTP + local timezone, so log records carry wall-clock timestamps.
 * Non-fatal: the logger falls back to uptime stamps when this fails. */
esp_err_t wifi_time_sync(uint32_t timeout_ms);
