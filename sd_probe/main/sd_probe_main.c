/* ESP32-P4-WIFI6-POE-ETH TF card probe.
 *
 * Verification history for this board:
 *   - 4-bit SDIO: CMD=GPIO44 CLK=GPIO43 D0=39 D1=40 D2=41 D3=42
 *   - card rail switched by Q1 (AO3401), gate = GPIO45 with 10k pull-down
 *   - 51k pull-ups go to the P4 on-chip LDO channel 4 (ESP_LDO_VO4)
 *
 * Step 1: initialise the card and print CID/CSD (proves wiring + power).
 * Step 2: dump the MBR and the first partition's boot sector so we can see
 *         which filesystem the card actually carries (ESP-IDF FatFs here is
 *         built with FF_FS_EXFAT = 0, so exFAT volumes report FR_NO_FILESYSTEM).
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_log.h"

static const char *TAG = "sd_probe";

#define PIN_CLK 43
#define PIN_CMD 44
#define PIN_D0  39
#define PIN_D1  40
#define PIN_D2  41
#define PIN_D3  42
#define PIN_SD_EN 45

static void print_ascii(const char *label, const uint8_t *p, int n)
{
    char out[64];
    int k = 0;
    for (int i = 0; i < n && k < (int)sizeof(out) - 1; i++) {
        out[k++] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '.';
    }
    out[k] = 0;
    printf("%s: '%s'\n", label, out);
}

static const char *fs_from_type(uint8_t t)
{
    switch (t) {
        case 0x01: return "FAT12";
        case 0x04: return "FAT16 <32M";
        case 0x06: return "FAT16";
        case 0x07: return "exFAT/NTFS";
        case 0x0b: return "FAT32 (CHS)";
        case 0x0c: return "FAT32 (LBA)";
        case 0x0e: return "FAT16 (LBA)";
        case 0xee: return "GPT protective";
        case 0x83: return "Linux";
        default:   return "unknown";
    }
}

void app_main(void)
{
    printf("\n=== TF card probe (CMD44 CLK43 D0..D3 39-42, power gate GPIO45) ===\n");

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_SD_EN,
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(PIN_SD_EN, 0);          /* Q1 gate low -> SD1_VDD on */
    vTaskDelay(pdMS_TO_TICKS(250));

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = 400;

    sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = 4 };
    sd_pwr_ctrl_handle_t pwr = NULL;
    if (sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr) == ESP_OK) {
        host.pwr_ctrl_handle = pwr;
    }

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = PIN_CLK; slot.cmd = PIN_CMD;
    slot.d0 = PIN_D0; slot.d1 = PIN_D1; slot.d2 = PIN_D2; slot.d3 = PIN_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_ERROR_CHECK(sdmmc_host_init());
    ESP_ERROR_CHECK(sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot));

    sdmmc_card_t card;
    memset(&card, 0, sizeof(card));
    esp_err_t err = sdmmc_card_init(&host, &card);
    printf("PROBE sdmmc_card_init -> %s (%d)\n", esp_err_to_name(err), err);
    if (err != ESP_OK) {
        printf("=== NO CARD RESPONSE ===\n");
        return;
    }
    sdmmc_card_print_info(stdout, &card);

    static uint8_t buf[512];
    /* ---- MBR ---- */
    err = sdmmc_read_sectors(&card, buf, 0, 1);
    if (err != ESP_OK) { printf("read MBR failed: %s\n", esp_err_to_name(err)); return; }

    printf("\n--- sector 0 (MBR) ---\n");
    print_ascii("OEM/loader", buf + 3, 8);
    printf("signature 0x55AA: %s\n", (buf[510] == 0x55 && buf[511] == 0xAA) ? "yes" : "no");
    uint32_t part_lba = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = buf + 446 + i * 16;
        uint32_t lba = e[8] | (e[9] << 8) | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        uint32_t cnt = e[12] | (e[13] << 8) | ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
        if (e[4] == 0 && cnt == 0) { printf("part%d: empty\n", i + 1); continue; }
        printf("part%d: type=0x%02x (%s) startLBA=%u sectors=%u (~%u MB)\n",
               i + 1, e[4], fs_from_type(e[4]), (unsigned)lba, (unsigned)cnt,
               (unsigned)((uint64_t)cnt * 512 / 1048576));
        if (part_lba == 0) { part_lba = lba; }
    }

    /* ---- volume boot record ---- */
    uint32_t vbr = part_lba;   /* superfloppy if no partition table */
    if (vbr != 0) {
        err = sdmmc_read_sectors(&card, buf, vbr, 1);
        if (err != ESP_OK) { printf("read VBR failed: %s\n", esp_err_to_name(err)); return; }
    }
    printf("\n--- boot sector at LBA %u ---\n", (unsigned)vbr);
    print_ascii("fs type string", buf + 3, 8);
    print_ascii("fs type string(0x52)", buf + 0x52, 8);
    print_ascii("fs type string(0x82)", buf + 0x82, 8);
    printf("bytes/sector=%u sectors/cluster=%u reserved=%u fats=%u\n",
           buf[11] | (buf[12] << 8), buf[13], buf[14] | (buf[15] << 8), buf[16]);
    printf("=== DONE ===\n");
}
