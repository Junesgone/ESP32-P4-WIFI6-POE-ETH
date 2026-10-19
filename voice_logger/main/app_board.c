/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#include "app_board.h"
#include "board_config.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

esp_err_t board_init(void)
{
    gpio_config_t pa = {
        .pin_bit_mask = 1ULL << BSP_PA_ENABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&pa);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PA gpio config failed: %s", esp_err_to_name(err));
        return err;
    }
    /* Amplifier stays muted until the codec is fully programmed, otherwise the
     * ES8311 power-on transient is audible. */
    gpio_set_level(BSP_PA_ENABLE, 0);
    ESP_LOGI(TAG, "board ready (PA_EN=GPIO%d held low)", BSP_PA_ENABLE);
    return ESP_OK;
}

void board_pa_enable(bool on)
{
    gpio_set_level(BSP_PA_ENABLE, on ? 1 : 0);
}

/* Simple square-wave chirp generated straight into the I2S TX path, so it does
 * not depend on the SD card or on Wi-Fi being up. */
void board_beep(void)
{
    extern esp_err_t audio_write_tone(int freq_hz, int ms, int amplitude);
    audio_write_tone(880, 120, 6000);
    vTaskDelay(pdMS_TO_TICKS(60));
    audio_write_tone(1320, 160, 6000);
}
