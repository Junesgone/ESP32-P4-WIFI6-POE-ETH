/* Ask the onboard ESP32-C6 what 2.4 GHz APs it can actually see.
 * The ESP32-C6 radio is 2.4 GHz only, so a 5 GHz-only SSID will never show up here. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    printf("\n=== asking the ESP32-C6 for a 2.4 GHz scan ===\n");
    vTaskDelay(pdMS_TO_TICKS(3000));

    wifi_scan_config_t scan = { .show_hidden = true };
    ESP_ERROR_CHECK(esp_wifi_scan_start(&scan, true));

    uint16_t n = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&n));
    printf("SCAN RESULT: %u AP(s) visible to the C6\n", n);
    if (n) {
        wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
        if (recs) {
            ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&n, recs));
            for (int i = 0; i < n; i++) {
                printf("  %2d) ssid='%s'  rssi=%d dBm  ch=%u  auth=%d\n",
                       i + 1, (const char *)recs[i].ssid, recs[i].rssi,
                       recs[i].primary, recs[i].authmode);
            }
            free(recs);
        }
    }
    printf("=== SCAN DONE ===\n");
    fflush(stdout);
}
