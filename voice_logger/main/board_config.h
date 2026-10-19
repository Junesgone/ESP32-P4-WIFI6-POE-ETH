/* Waveshare ESP32-P4-WIFI6-POE-ETH board map.
 * Every pin below was read off the board schematic and confirmed on hardware.
 */
#pragma once

#include "driver/gpio.h"

/* ---------------- ES8311 codec + NS4150B amplifier (speaker + on-board mic) ---- */
#define BSP_I2C_SDA          GPIO_NUM_7    /* ESP_I2C_SDA */
#define BSP_I2C_SCL          GPIO_NUM_8    /* ESP_I2C_SCL */
#define BSP_I2S_MCLK         GPIO_NUM_13   /* I2S_MCLK  */
#define BSP_I2S_BCLK         GPIO_NUM_12   /* I2S_SCLK  */
#define BSP_I2S_WS           GPIO_NUM_10   /* I2S_LRCK  */
#define BSP_I2S_DOUT         GPIO_NUM_9    /* P4 -> codec (I2S_DSDIN) */
#define BSP_I2S_DIN          GPIO_NUM_11   /* codec -> P4 (I2S_ASDOUT) */
#define BSP_PA_ENABLE        GPIO_NUM_53   /* NS4150B CTRL through R71 0R */
#define BSP_ES8311_ADDR      0x18          /* CE tied low */

/* The on-board microphone is an ANALOG differential MEMS capsule wired through
 * C72/C74 (1 uF) into the ES8311 MIC1P/MIC1N pins - there is no separate mic
 * driver, it is simply the codec ADC path read on I2S DIN. */

/* ---------------- TF card (SD1) ----------------------------------------------
 * On ESP32-P4 the SD card must live on SDMMC slot 0 (fixed pins 39..44), while
 * the on-board ESP32-C6 that provides Wi-Fi sits on SDMMC slot 1.
 */
#define BSP_SD_CLK           GPIO_NUM_43
#define BSP_SD_CMD           GPIO_NUM_44
#define BSP_SD_D0            GPIO_NUM_39
#define BSP_SD_D1            GPIO_NUM_40
#define BSP_SD_D2            GPIO_NUM_41
#define BSP_SD_D3            GPIO_NUM_42
#define BSP_SD_LDO_CHAN      4             /* ESP_LDO_VO4 feeds the 51k pull-ups */
#define BSP_SD_MOUNT_POINT   "/sdcard"

/* ---------------- Reserved: MIPI-CSI camera / face recognition ---------------
 * The camera uses dedicated MIPI pins, but its SCCB control bus shares the codec
 * I2C bus, so the two must be brought up on the same port later.
 */
#define BSP_CAM_SCCB_SCL     BSP_I2C_SCL
#define BSP_CAM_SCCB_SDA     BSP_I2C_SDA
#define BSP_CAM_I2C_PORT     0
