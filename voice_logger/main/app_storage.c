/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#include "app_storage.h"
#include "board_config.h"

#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "esp_vfs_fat.h"
#include <sys/stat.h>
#include <time.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "storage";

/* The TF card is on SDMMC slot 0 while ESP-Hosted owns slot 1 for the ESP32-C6.
 * On IDF >= 6.0 the host controller is created exactly once; ESP-Hosted gets
 * there first, so the SD side must not create it again.  Mirrors the workaround
 * in esp_hosted's host_sdcard_with_hosted example. */
#if CONFIG_ESP_HOSTED_ENABLED && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0))
#define HOSTED_OWNS_SDMMC_HOST 1
#else
#define HOSTED_OWNS_SDMMC_HOST 0
#endif

static sdmmc_card_t *s_card;
static sd_pwr_ctrl_handle_t s_pwr;
static bool s_ready;
static bool s_time_synced;

#if HOSTED_OWNS_SDMMC_HOST
static esp_err_t sdmmc_host_init_dummy(void) { return ESP_OK; }
static esp_err_t sdmmc_host_deinit_dummy(void) { return ESP_OK; }
#endif

esp_err_t storage_mount(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;          /* required on ESP32-P4 */
    host.max_freq_khz = 20000;
#if HOSTED_OWNS_SDMMC_HOST
    host.init = &sdmmc_host_init_dummy;
    host.deinit = &sdmmc_host_deinit_dummy;
#endif

    sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = BSP_SD_LDO_CHAN };
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_pwr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "on-chip LDO%d power control failed: %s", BSP_SD_LDO_CHAN, esp_err_to_name(err));
        return err;
    }
    host.pwr_ctrl_handle = s_pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = BSP_SD_CLK;
    slot.cmd = BSP_SD_CMD;
    slot.d0  = BSP_SD_D0;
    slot.d1  = BSP_SD_D1;
    slot.d2  = BSP_SD_D2;
    slot.d3  = BSP_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    err = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot, &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s (card present? formatted FAT32?)", esp_err_to_name(err));
        return err;
    }

    sdmmc_card_print_info(stdout, s_card);
    mkdir(BSP_SD_MOUNT_POINT "/conv", 0777);
    s_ready = true;
    ESP_LOGI(TAG, "TF card mounted at %s", BSP_SD_MOUNT_POINT);
    return ESP_OK;
}

bool storage_ready(void)
{
    return s_ready;
}

void storage_set_time_synced(bool synced) { s_time_synced = synced; }
bool storage_time_synced(void) { return s_time_synced; }

void storage_timestamp(char *buf, size_t len)
{
    if (!s_time_synced) {
        snprintf(buf, len, "uptime+%llums",
                 (unsigned long long)(xTaskGetTickCount() * portTICK_PERIOD_MS));
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%S%z", &tm);
}

static void wav_header(uint8_t *h, uint32_t data_bytes, uint32_t rate)
{
    uint32_t byte_rate = rate * 2;
    uint16_t block_align = 2, channels = 1, bits = 16, pcm = 1;
    uint32_t fmt_len = 16, riff = 36 + data_bytes;
    memcpy(h + 0, "RIFF", 4);
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    memcpy(h + 16, &fmt_len, 4);
    memcpy(h + 20, &pcm, 2);
    memcpy(h + 22, &channels, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &block_align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
}

static void json_escape(char *dst, size_t dst_len, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 7 < dst_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c == '\n') {
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else if (c == '\r') {
            /* drop */
        } else if (c == '\t') {
            dst[j++] = '\\';
            dst[j++] = 't';
        } else if (c < 0x20) {
            j += (size_t)snprintf(dst + j, dst_len - j, "\\u%04x", c);
        } else {
            dst[j++] = (char)c;
        }
    }
    dst[j] = 0;
}

esp_err_t storage_save_wav(const int16_t *mono, size_t samples, uint32_t sample_rate,
                           uint32_t duration_ms,
                           char *out_rel_path, size_t out_rel_path_len)
{
    /* Card init occasionally fails at boot (it shares the SDMMC peripheral with
     * the ESP-Hosted SDIO link to the C6).  Rather than silently dropping the
     * recording, try once more before giving up. */
    if (!s_ready) {
        ESP_LOGW(TAG, "card not mounted - retrying before saving");
        storage_mount();
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    char dir[64], name[32];
    if (s_time_synced) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(dir, sizeof(dir), "conv/%Y%m%d", &tm);
        strftime(name, sizeof(name), "%H%M%S.wav", &tm);
    } else {
        snprintf(dir, sizeof(dir), "conv/boot");
        snprintf(name, sizeof(name), "%08llu.wav",
                 (unsigned long long)(xTaskGetTickCount() * portTICK_PERIOD_MS));
    }

    char abs_dir[96], abs_path[128], rel_path[128];
    snprintf(abs_dir, sizeof(abs_dir), "%s/%s", BSP_SD_MOUNT_POINT, dir);
    if (mkdir(abs_dir, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed: errno %d", abs_dir, errno);
    }
    snprintf(abs_path, sizeof(abs_path), "%s/%s", abs_dir, name);
    snprintf(rel_path, sizeof(rel_path), "%s/%s", dir, name);

    uint32_t data_bytes = (uint32_t)(samples * sizeof(int16_t));
    FILE *f = fopen(abs_path, "wb");
    if (!f) {
        /* Report why, and how much room is left - a card that has gone
         * read-only or filled up looks identical otherwise. */
        uint64_t total = 0, freeb = 0;
        unsigned long long free_mb = 0;
        if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &freeb) == ESP_OK) {
            free_mb = (unsigned long long)(freeb >> 20);
        }
        ESP_LOGE(TAG, "cannot create %s: errno %d, mounted=%d, card free %llu MB",
                 abs_path, errno, (int)s_ready, free_mb);
        return ESP_FAIL;
    }
    uint8_t hdr[44];
    wav_header(hdr, data_bytes, sample_rate);
    fwrite(hdr, 1, sizeof(hdr), f);
    size_t wrote = fwrite(mono, 1, data_bytes, f);
    fclose(f);
    if (wrote != data_bytes) {
        ESP_LOGE(TAG, "short write on %s", abs_path);
        return ESP_FAIL;
    }
    if (out_rel_path && out_rel_path_len) {
        snprintf(out_rel_path, out_rel_path_len, "%s", rel_path);
    }
    ESP_LOGI(TAG, "wav %s (%u ms)", rel_path, (unsigned)duration_ms);
    return ESP_OK;
}

esp_err_t storage_recent_text(char *out, size_t out_len, int max_records)
{
    if (!s_ready || !out || out_len < 8) {
        return ESP_ERR_INVALID_STATE;
    }
    out[0] = 0;
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", BSP_SD_MOUNT_POINT, STORAGE_INDEX_PATH);
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(out, out_len, "还没有任何记录。");
        return ESP_OK;
    }

    enum { MAXL = 16, LINELEN = 900 };
    static char ring[MAXL][LINELEN];
    int count = 0, head = 0;
    char buf[LINELEN];
    while (fgets(buf, sizeof(buf), f)) {
        snprintf(ring[head], LINELEN, "%s", buf);
        head = (head + 1) % MAXL;
        if (count < MAXL) count++;
    }
    fclose(f);

    int show = max_records < count ? max_records : count;
    size_t off = 0;
    for (int i = count - show; i < count && off + 8 < out_len; i++) {
        const char *line = ring[((head - count + i) % MAXL + MAXL) % MAXL];
        /* Pull just the time/text/reply/speaker fields out of the JSON. */
        char ts[40] = "", text[220] = "", reply[280] = "", spk[40] = "";
        const char *p;
        if ((p = strstr(line, "\"ts\":\"")) != NULL) sscanf(p + 6, "%39[^\"]", ts);
        if ((p = strstr(line, "\"text\":\"")) != NULL) sscanf(p + 8, "%219[^\"]", text);
        if ((p = strstr(line, "\"reply\":\"")) != NULL) sscanf(p + 9, "%279[^\"]", reply);
        if ((p = strstr(line, "\"speaker_id\":\"")) != NULL) sscanf(p + 14, "%39[^\"]", spk);
        off += snprintf(out + off, out_len - off, "%s [%s] 说：%s → 小新：%s\n",
                        ts[0] ? ts : "?", spk[0] ? spk : "未知", text, reply);
    }
    return ESP_OK;
}

esp_err_t storage_dump_index(int max_lines)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", BSP_SD_MOUNT_POINT, STORAGE_INDEX_PATH);
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("(no index yet at %s)\n", path);
        return ESP_FAIL;
    }

    /* Keep a rolling window of the last N lines, then print them. */
    enum { MAXL = 32, LINELEN = 900 };
    static char ring[MAXL][LINELEN];
    int count = 0;
    int head = 0;
    char buf[LINELEN];
    while (fgets(buf, sizeof(buf), f)) {
        snprintf(ring[head], LINELEN, "%s", buf);
        head = (head + 1) % MAXL;
        if (count < MAXL) {
            count++;
        }
    }
    fclose(f);

    int show = max_lines < count ? max_lines : count;
    printf("--- last %d record(s) of %s ---\n", show, STORAGE_INDEX_PATH);
    for (int i = count - show; i < count; i++) {
        printf("%s", ring[((head - count + i) % MAXL + MAXL) % MAXL]);
    }
    printf("--- end (%d total buffered) ---\n", count);
    return ESP_OK;
}

esp_err_t storage_append_turn(const char *rel_wav, uint32_t duration_ms, int rms_peak,
                              const char *face_id, const char *speaker_id,
                              const char *text, const char *reply)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    char idx_abs[96];
    snprintf(idx_abs, sizeof(idx_abs), "%s/%s", BSP_SD_MOUNT_POINT, STORAGE_INDEX_PATH);

    char stamp[40];
    storage_timestamp(stamp, sizeof(stamp));

    /* Escaped copies live on the heap: heap blow-up on a 3 KB reply would
     * otherwise sit on the main task stack. */
    char *t_esc = malloc(4096);
    char *r_esc = malloc(6400);
    char *f_esc = malloc(80);
    char *s_esc = malloc(80);
    if (!t_esc || !r_esc || !f_esc || !s_esc) {
        free(t_esc); free(r_esc); free(f_esc); free(s_esc);
        return ESP_ERR_NO_MEM;
    }
    json_escape(t_esc, 4096, text);
    json_escape(r_esc, 6400, reply);
    json_escape(f_esc, 80, face_id);
    json_escape(s_esc, 80, speaker_id);

    FILE *j = fopen(idx_abs, "a");
    if (!j) {
        ESP_LOGW(TAG, "cannot append %s", idx_abs);
        free(t_esc); free(r_esc); free(f_esc); free(s_esc);
        return ESP_FAIL;
    }
    fprintf(j,
            "{\"schema\":1,\"ts\":\"%s\",\"uptime_ms\":%llu,\"wav\":\"%s\","
            "\"duration_ms\":%u,\"rms_peak\":%d,"
            "\"face_id\":\"%s\",\"speaker_id\":\"%s\","
            "\"text\":\"%s\",\"reply\":\"%s\"}\n",
            stamp,
            (unsigned long long)(xTaskGetTickCount() * portTICK_PERIOD_MS),
            rel_wav ? rel_wav : "", (unsigned)duration_ms, rms_peak,
            f_esc, s_esc, t_esc, r_esc);
    fclose(j);
    free(t_esc); free(r_esc); free(f_esc); free(s_esc);
    return ESP_OK;
}
