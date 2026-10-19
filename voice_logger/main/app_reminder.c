/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#include "app_reminder.h"

#include <stdio.h>
#include <string.h>

#include "app_tts.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "remind";

#define MAX_REMINDERS 8
#define REMINDER_TEXT_LEN 96

typedef struct {
    bool    used;
    int     id;
    int64_t due_us;
    char    text[REMINDER_TEXT_LEN];
} reminder_t;

static reminder_t s_rem[MAX_REMINDERS];
static int s_next_id = 1;
static SemaphoreHandle_t s_lock;

static void reminder_task(void *arg)
{
    (void)arg;
    while (1) {
        int64_t now = esp_timer_get_time();
        char fired[MAX_REMINDERS][REMINDER_TEXT_LEN];
        int nfired = 0;

        if (s_lock) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
        }
        for (int i = 0; i < MAX_REMINDERS; i++) {
            if (s_rem[i].used && now >= s_rem[i].due_us) {
                if (nfired < MAX_REMINDERS) {
                    snprintf(fired[nfired], REMINDER_TEXT_LEN, "%s", s_rem[i].text);
                    nfired++;
                }
                s_rem[i].used = false;
            }
        }
        if (s_lock) {
            xSemaphoreGive(s_lock);
        }

        for (int i = 0; i < nfired; i++) {
            char say[REMINDER_TEXT_LEN + 16];
            snprintf(say, sizeof(say), "提醒：%.80s", fired[i]);
            ESP_LOGW(TAG, "%s", say);
            if (tts_ready()) {
                tts_say(say);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t reminder_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    /* 4 KB was enough while reminders only rendered ESP-SR locally.  A cloud
     * TTS announcement runs a TLS session on this task, so give it room. */
    BaseType_t ok = xTaskCreate(reminder_task, "remind", 12288, NULL, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task creation failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "reminder scheduler ready (up to %d)", MAX_REMINDERS);
    return ESP_OK;
}

int reminder_add(uint32_t seconds, const char *text)
{
    if (!text || !text[0]) {
        return -1;
    }
    if (seconds < 1) {
        seconds = 1;
    }
    int id = -1;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    for (int i = 0; i < MAX_REMINDERS; i++) {
        if (!s_rem[i].used) {
            s_rem[i].used = true;
            s_rem[i].id = s_next_id++;
            s_rem[i].due_us = esp_timer_get_time() + (int64_t)seconds * 1000000LL;
            snprintf(s_rem[i].text, sizeof(s_rem[i].text), "%s", text);
            id = s_rem[i].id;
            break;
        }
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    if (id < 0) {
        ESP_LOGW(TAG, "table full, reminder dropped");
    } else {
        ESP_LOGI(TAG, "reminder #%d in %u s: %s", id, (unsigned)seconds, text);
    }
    return id;
}

int reminder_list(char *out, size_t out_len)
{
    if (!out || !out_len) {
        return 0;
    }
    int64_t now = esp_timer_get_time();
    int off = 0;
    int n = 0;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    for (int i = 0; i < MAX_REMINDERS && off < (int)out_len - 40; i++) {
        if (!s_rem[i].used) {
            continue;
        }
        int64_t left_s = (s_rem[i].due_us - now) / 1000000LL;
        if (left_s < 0) {
            left_s = 0;
        }
        off += snprintf(out + off, out_len - (size_t)off, "%d 秒后 %s\n",
                        (int)left_s, s_rem[i].text);
        n++;
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    if (n == 0) {
        snprintf(out, out_len, "没有待触发的提醒。");
    }
    return n;
}

int reminder_cancel_all(void)
{
    int n = 0;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    for (int i = 0; i < MAX_REMINDERS; i++) {
        if (s_rem[i].used) {
            s_rem[i].used = false;
            n++;
        }
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "cancelled %d reminder(s)", n);
    return n;
}

int reminder_pending_count(void)
{
    int n = 0;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    for (int i = 0; i < MAX_REMINDERS; i++) {
        if (s_rem[i].used) {
            n++;
        }
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    return n;
}
