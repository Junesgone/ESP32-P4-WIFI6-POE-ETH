# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Junesgone

idf_component_register(SRCS "main.c"
                       "app_board.c"
                       "app_audio.c"
                       "app_vad.c"
                       "app_storage.c"
                       "app_wifi.c"
                       "app_identity.c"
                       "app_cloud.c"
                       "app_asr.c"
                       "app_tts.c"
                       "app_tts_cloud.c"
                       "app_speaker_id.cpp"
                       "app_tools.c"
                       "app_aec.c"
                       "app_ble.c"
                       "app_deepseek.c"
                       "app_console.c"
                       "app_reminder.c"
                       "app_conversation.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_driver_i2s esp_driver_gpio esp_driver_i2c
                                   es8311 fatfs sdmmc esp_driver_sdmmc
                                   esp_wifi nvs_flash esp_netif esp_timer
                                   esp_http_client esp_http_server mbedtls esp-tls
                                   cjson esp-dl bt)

# esp-dl is now used only for its Fbank feature front end, so no model op
# declaration is needed here any more (esp_dl_compile_add_yml was required only
# by the espressif/speaker_verification TDNN, which has been dropped).
