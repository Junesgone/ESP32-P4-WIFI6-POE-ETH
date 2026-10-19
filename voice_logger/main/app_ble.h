/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#pragma once
/* BLE through the on-board ESP32-C6.
 *
 * The C6 already ships as a BT controller (BT_CONTROLLER_ONLY), so nothing has
 * to be reflashed on it - the P4 runs NimBLE and reaches the controller over
 * VHCI on the same SDIO link Wi-Fi uses.
 *
 * This module plays the observer/central role: it can list what is
 * advertising nearby and open a link to one of them.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t ble_init(void);
bool ble_ready(void);

/* Scans for duration_ms and fills the arrays (strongest first).
 * Returns the number found, or -1 on error. */
int ble_scan(char names[][32], uint8_t addrs[][6], int8_t *rssi, int max,
             uint32_t duration_ms);

/* Opens a link to a peer found by ble_scan. */
esp_err_t ble_connect(const uint8_t addr[6]);
esp_err_t ble_disconnect(void);
bool ble_connected(void);
