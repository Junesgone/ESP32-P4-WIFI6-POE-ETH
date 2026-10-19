/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#include "app_console.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "console";

static console_line_handler_t s_handler;

static void console_task(void *arg)
{
    char line[512];
    ESP_LOGI(TAG, "type a line + Enter to talk to DeepSeek");
    while (1) {
        if (!fgets(line, sizeof(line), stdin)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = 0;
        }
        if (n == 0) {
            continue;
        }
        if (s_handler) {
            s_handler(line);
        }
    }
}

void console_start(console_line_handler_t handler)
{
    s_handler = handler;
    ESP_LOGI(TAG, "internal heap before console task: %u free, largest block %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    /* TLS handshakes happen on this task, so give mbedtls plenty of stack.
     * A silent failure here used to kill the whole serial console: the board
     * looked alive (audio, VAD, Wi-Fi) but no command was ever answered. */
    BaseType_t ok = xTaskCreate(console_task, "console", 16384, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "console task creation FAILED (%d) - no serial commands", (int)ok);
        ok = xTaskCreate(console_task, "console", 10240, NULL, 4, NULL);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "console task retry FAILED too (%d)", (int)ok);
        } else {
            ESP_LOGW(TAG, "console running with a reduced 10 KB stack");
        }
    }
}
