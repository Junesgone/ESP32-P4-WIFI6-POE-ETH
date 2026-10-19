/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#include "app_aec.h"
#include "app_audio.h"

#include <string.h>

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "model_path.h"

static const char *TAG = "aec";

static const esp_afe_sr_iface_t *s_iface;
static esp_afe_sr_data_t *s_afe;
static srmodel_list_t *s_models;
static int s_feed_fs;
static int s_feed_ch;
static int16_t *s_fetched;
static int s_fetched_len;

esp_err_t aec_init(void)
{
    /* ESP-SR packs the models it needs into the "model" partition and its build
     * system flashes them automatically (see esp_sr/CMakeLists.txt). */
    s_models = esp_srmodel_init("model");
    if (!s_models) {
        ESP_LOGE(TAG, "no models in the 'model' partition - run idf.py flash");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "%d esp-sr model(s) available", s_models->num);

    /* "MR" = one Microphone channel plus one playback Reference channel. */
    afe_config_t *cfg = afe_config_init("MR", s_models, AFE_TYPE_VC,
                                        AFE_MODE_LOW_COST);
    if (!cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }

    cfg->aec_init = true;
    cfg->aec_mode = AEC_MODE_FD_LOW_COST;      /* full duplex: barge-in works */
    cfg->aec_filter_length = 4;                /* room tail; raise for a big room */
    cfg->aec_nlp_level = AEC_NLP_LEVEL_AGGR;   /* squash the residual echo */

    /* We keep our own energy VAD - it is already tuned (threshold 800) and it
     * feeds the log-mel voiceprint, so AFE's VAD/WakeNet stay off. */
    cfg->vad_init = false;
    cfg->wakenet_init = false;
    /* AGC instead of a fixed ES8311 gain: it lifts quiet speech without
     * clipping loud speech, and it is what the ASR uplink actually wants.
     * Measured before: speech peaks 1900-5900 vs a noise floor that drifts
     * between 20 and 950, so a fixed boost would either clip or be useless. */
    /* Compile-time only: the AFE's runtime AGC/NS ops are not usable in this
     * configuration.  Measured: turning AGC off did not improve the voiceprint
     * (cross-speaker cosine actually rose slightly), so both default to on. */
    cfg->agc_init = CONFIG_VOICE_AEC_AGC;
    cfg->ns_init = CONFIG_VOICE_AEC_NS;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    /* The AFE flavour (SR / VC / FD, low cost or high perf) is chosen by the
     * config, so the interface pointer has to be asked for it. */
    s_iface = esp_afe_handle_from_config(cfg);
    if (!s_iface) {
        ESP_LOGE(TAG, "no AFE handle for this config");
        afe_config_free(cfg);
        return ESP_FAIL;
    }
    s_afe = s_iface->create_from_config(cfg);
    afe_config_free(cfg);
    if (!s_afe) {
        ESP_LOGE(TAG, "AFE create failed");
        return ESP_FAIL;
    }

    s_feed_fs = s_iface->get_feed_chunksize(s_afe);
    s_feed_ch = s_iface->get_feed_channel_num(s_afe);
    s_fetched = (int16_t *)heap_caps_malloc(
        (size_t)s_iface->get_fetch_chunksize(s_afe) * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fetched) {
        ESP_LOGE(TAG, "no memory for fetch buffer");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "AFE up: feed %d samples x %d ch, fetch %d samples, AEC full-duplex",
             s_feed_fs, s_feed_ch, s_iface->get_fetch_chunksize(s_afe));
    if (s_iface->print_pipeline) {
        s_iface->print_pipeline(s_afe);
    }
    return ESP_OK;
}

bool aec_ready(void)
{
    return s_afe != NULL;
}

int aec_feed_chunksize(void)
{
    return s_feed_fs;
}

const int16_t *app_aec_process(const int16_t *interleaved, int *out_samples)
{
    if (out_samples) {
        *out_samples = 0;
    }
    if (!s_afe || !interleaved) {
        return NULL;
    }

    s_iface->feed(s_afe, interleaved);

    /* Block briefly: the AFE runs feed and fetch at the same cadence, so a
     * zero-tick fetch almost always finds the ring still empty and logs
     * "Ringbuffer of AFE is empty" on every iteration.  The wait also paces
     * the capture loop to real time. */
    afe_fetch_result_t *res = s_iface->fetch_with_delay(s_afe, pdMS_TO_TICKS(200));
    if (!res || !res->data || res->data_size <= 0) {
        return NULL;
    }
    int n = res->data_size / (int)sizeof(int16_t);
    if (n > s_iface->get_fetch_chunksize(s_afe)) {
        n = s_iface->get_fetch_chunksize(s_afe);
    }
    memcpy(s_fetched, res->data, (size_t)n * sizeof(int16_t));
    s_fetched_len = n;
    if (out_samples) {
        *out_samples = n;
    }
    return s_fetched;
}
