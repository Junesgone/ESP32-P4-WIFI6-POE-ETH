#include "app_audio.h"
#include "board_config.h"

#include <math.h>
#include <string.h>

#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "es8311.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#define I2C_PORT       I2C_NUM_0
#define MCLK_MULTIPLE  384

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static es8311_handle_t   s_codec;

esp_err_t audio_init(void)
{
    const i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_PORT, &i2c_cfg), TAG, "i2c_param_config");
    ESP_RETURN_ON_ERROR(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0), TAG, "i2c_driver_install");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_BCLK,
            .ws   = BSP_I2S_WS,
            .dout = BSP_I2S_DOUT,
            .din  = BSP_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = MCLK_MULTIPLE;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s tx init");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "i2s rx init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s tx enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "i2s rx enable");

    s_codec = es8311_create(I2C_PORT, ES8311_ADDRRES_0);
    ESP_RETURN_ON_FALSE(s_codec, ESP_FAIL, TAG, "es8311_create");

    const es8311_clock_config_t clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = AUDIO_SAMPLE_RATE * MCLK_MULTIPLE,
        .sample_frequency = AUDIO_SAMPLE_RATE,
    };
    ESP_RETURN_ON_ERROR(es8311_init(s_codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16),
                        TAG, "es8311_init");
    ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(s_codec, AUDIO_SAMPLE_RATE * MCLK_MULTIPLE,
                                                       AUDIO_SAMPLE_RATE),
                        TAG, "es8311_sample_frequency_config");
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set(s_codec, CONFIG_VOICE_SPEAKER_VOLUME, NULL),
                        TAG, "es8311 volume");
    /* The on-board capsule is an ANALOG differential mic into MIC1P/MIC1N. */
    ESP_RETURN_ON_ERROR(es8311_microphone_config(s_codec, false), TAG, "es8311_microphone_config");
    ESP_RETURN_ON_ERROR(es8311_voice_mute(s_codec, false), TAG, "es8311 unmute");

    ESP_LOGI(TAG, "ES8311 ready: %d Hz, %d-bit, stereo I2S, mic=analog, volume=%d",
             AUDIO_SAMPLE_RATE, 16, CONFIG_VOICE_SPEAKER_VOLUME);
    return ESP_OK;
}

esp_err_t audio_set_volume(int volume_0_100)
{
    ESP_RETURN_ON_FALSE(s_codec, ESP_ERR_INVALID_STATE, TAG, "codec not ready");
    return es8311_voice_volume_set(s_codec, volume_0_100, NULL);
}

int audio_get_volume(void)
{
    int v = 0;
    if (s_codec && es8311_voice_volume_get(s_codec, &v) == ESP_OK) {
        return v;
    }
    return -1;
}

void audio_set_mic_gain_db(int gain_db)
{
    es8311_mic_gain_t g = ES8311_MIC_GAIN_0DB;
    switch (gain_db) {
        case 6:  g = ES8311_MIC_GAIN_6DB; break;
        case 12: g = ES8311_MIC_GAIN_12DB; break;
        case 18: g = ES8311_MIC_GAIN_18DB; break;
        case 24: g = ES8311_MIC_GAIN_24DB; break;
        case 30: g = ES8311_MIC_GAIN_30DB; break;
        case 36: g = ES8311_MIC_GAIN_36DB; break;
        case 42: g = ES8311_MIC_GAIN_42DB; break;
        default: break;
    }
    if (s_codec) {
        es8311_microphone_gain_set(s_codec, g);
    }
}

esp_err_t audio_read(int16_t *dst, size_t bytes, size_t *bytes_read, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_rx, ESP_ERR_INVALID_STATE, TAG, "rx not ready");
    return i2s_channel_read(s_rx, dst, bytes, bytes_read, pdMS_TO_TICKS(timeout_ms));
}

/* Ring holding the last second of playback, drained by the AEC. */
#define REF_RING_SAMPLES 16384
static int16_t s_ref_ring[REF_RING_SAMPLES];
static volatile int s_ref_head;
static volatile int s_ref_tail;

/* ---------------------- AEC reference delay line -------------------------
 * The reference enters the canceller the instant audio_write() is called, but
 * the sound only leaves the speaker after the I2S DMA drains and the codec
 * clocking latency, so the canceller is fed the reference *too early* and
 * cancels nothing.  That is why the board used to transcribe its own replies.
 *
 * This circular buffer holds the reference back by the measured number of
 * samples so mic and reference line up.  The value is produced by the 'calib'
 * console command (see main.c) and persisted in NVS. */
#define REF_DELAY_MAX 3200          /* 200 ms at 16 kHz */
static int16_t s_ref_delay_buf[REF_DELAY_MAX];
static int s_ref_delay;
static int s_ref_delay_pos;
static int s_ref_delay_fill;

void audio_ref_set_delay(int samples)
{
    if (samples < 0) {
        samples = 0;
    }
    if (samples > REF_DELAY_MAX) {
        samples = REF_DELAY_MAX;
    }
    s_ref_delay = samples;
    s_ref_delay_pos = 0;
    s_ref_delay_fill = 0;
    memset(s_ref_delay_buf, 0, sizeof(s_ref_delay_buf));
}

int audio_ref_get_delay(void)
{
    return s_ref_delay;
}

static inline int16_t ref_delay_sample(int16_t v)
{
    if (s_ref_delay <= 0) {
        return v;
    }
    int16_t out = s_ref_delay_buf[s_ref_delay_pos];
    s_ref_delay_buf[s_ref_delay_pos] = v;
    s_ref_delay_pos = (s_ref_delay_pos + 1) % s_ref_delay;
    if (s_ref_delay_fill < s_ref_delay) {
        s_ref_delay_fill++;
        out = 0;    /* the delay line has not filled yet */
    }
    return out;
}

static void ref_push(const int16_t *stereo, size_t frames)
{
    for (size_t i = 0; i < frames; i++) {
        int next = (s_ref_head + 1) % REF_RING_SAMPLES;
        if (next == s_ref_tail) {
            s_ref_tail = (s_ref_tail + 1) % REF_RING_SAMPLES;   /* drop oldest */
        }
        s_ref_ring[s_ref_head] = ref_delay_sample(stereo[i * AUDIO_CHANNELS]);
        s_ref_head = next;
    }
}

int audio_ref_pop(int16_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        if (s_ref_tail == s_ref_head) {
            out[i] = 0;     /* nothing playing: the reference really is silence */
            continue;
        }
        out[i] = s_ref_ring[s_ref_tail];
        s_ref_tail = (s_ref_tail + 1) % REF_RING_SAMPLES;
    }
    return n;
}

esp_err_t audio_write(const int16_t *src, size_t bytes)
{
    ESP_RETURN_ON_FALSE(s_tx, ESP_ERR_INVALID_STATE, TAG, "tx not ready");
    ref_push(src, bytes / (AUDIO_CHANNELS * sizeof(int16_t)));
    size_t written = 0;
    return i2s_channel_write(s_tx, src, bytes, &written, pdMS_TO_TICKS(1000));
}

void audio_drain_rx(void)
{
    static int16_t scratch[512 * AUDIO_CHANNELS];
    size_t got = 0;
    int guard = 64;
    while (guard-- > 0) {
        if (i2s_channel_read(s_rx, scratch, sizeof(scratch), &got, 0) != ESP_OK || got == 0) {
            break;
        }
    }
}

void audio_downmix(const int16_t *stereo, size_t stereo_samples, int16_t *mono)
{
    size_t frames = stereo_samples / AUDIO_CHANNELS;
    for (size_t i = 0; i < frames; i++) {
        int32_t l = stereo[i * AUDIO_CHANNELS];
        int32_t r = stereo[i * AUDIO_CHANNELS + 1];
        mono[i] = (int16_t)((l + r) / 2);
    }
}

esp_err_t audio_write_tone(int freq_hz, int ms, int amplitude)
{
    const int frames = (AUDIO_SAMPLE_RATE * ms) / 1000;
    enum { CHUNK = 256 };
    int16_t buf[CHUNK * AUDIO_CHANNELS];
    int done = 0;
    double phase = 0.0;
    const double step = 2.0 * M_PI * (double)freq_hz / (double)AUDIO_SAMPLE_RATE;

    while (done < frames) {
        int n = frames - done;
        if (n > CHUNK) {
            n = CHUNK;
        }
        for (int i = 0; i < n; i++) {
            int16_t s = (int16_t)(sin(phase) * amplitude);
            buf[i * 2] = s;
            buf[i * 2 + 1] = s;
            phase += step;
            if (phase > 2.0 * M_PI) {
                phase -= 2.0 * M_PI;
            }
        }
        esp_err_t err = audio_write(buf, (size_t)n * AUDIO_CHANNELS * sizeof(int16_t));
        if (err != ESP_OK) {
            return err;
        }
        done += n;
    }
    return ESP_OK;
}
