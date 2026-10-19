/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

/* Voice logger for the Waveshare ESP32-P4-WIFI6-POE-ETH.
 *
 *   on-board analog mic -> ES8311 ADC -> I2S RX -> energy VAD
 *        -> mono 16 kHz WAV on the TF card + one JSONL index line
 *        -> optional playback through ES8311 DAC + NS4150B
 *
 * Wi-Fi (via the on-board ESP32-C6 over ESP-Hosted/SDIO) is only used for SNTP
 * wall-clock stamps; the capture path keeps working with or without a network.
 *
 * Identity fields in each record come from app_identity.c.  No camera is
 * attached yet, so they read "unknown" - that is the seam where ESP-DL / ESP-WHO
 * face recognition and speaker verification plug in later.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "esp_vfs_fat.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "app_aec.h"
#include "app_asr.h"
#include "app_audio.h"
#include "app_ble.h"
#include "app_board.h"
#include "app_console.h"
#include "app_conversation.h"
#include "app_deepseek.h"
#include "app_reminder.h"
#include "app_identity.h"
#include "app_speaker_id.h"
#include "app_storage.h"
#include "app_tools.h"
#include "app_tts.h"
#include "app_tts_cloud.h"
#include "app_vad.h"
#include "app_wifi.h"
#include "board_config.h"

static const char *TAG = "voice";

#define FRAME_MS       20
#define FRAME_SAMPLES  (AUDIO_SAMPLE_RATE * FRAME_MS / 1000)   /* 320, plain I2S path */
#define FRAME_STEREO   (FRAME_SAMPLES * AUDIO_CHANNELS)
/* The AFE hands back 512 samples per frame, not 320.  Every buffer below is
 * sized from s_frame at runtime - sizing them from FRAME_SAMPLES while the AEC
 * path delivered 512 is what caused a heap overflow on the first utterance
 * (Load access fault, reboot, greeting again). */
static size_t   s_frame;
#define PREROLL_FRAMES (CONFIG_VOICE_PREROLL_MS / FRAME_MS)
#define UTT_MAX_SAMPLES ((AUDIO_SAMPLE_RATE * CONFIG_VOICE_VAD_MAX_UTTERANCE_MS) / 1000 + AUDIO_SAMPLE_RATE)

static int16_t *s_stereo;
static int16_t *s_mono;
static int16_t *s_preroll;          /* PREROLL_FRAMES * s_frame, AFE output */
static int16_t *s_utt;              /* UTT_MAX_SAMPLES, AFE output (ASR/storage) */
static size_t   s_utt_len;
/* Parallel copy of the same audio taken BEFORE the AFE.  The voiceprint is
 * computed from this one: AEC/NS/AGC reshape the spectrum enough to flatten
 * speaker timbre (measured 2026-10-19: cross-speaker cosine 0.52-0.71 on AFE
 * output vs a clean separation expected from the offline lab). */
static int16_t *s_preroll_raw;
static int16_t *s_utt_raw;
static size_t   s_utt_raw_len;
static int      s_rms_peak;
static int      s_preroll_head;
static int      s_preroll_count;
static unsigned s_utterance_no;
/* While the speaker is still ringing (and the room is still echoing), the mic
 * hears our own reply.  There is no AEC in this build, so simply ignore input
 * for a moment after each spoken reply - otherwise the board talks to itself. */
static int64_t s_suppress_until_us;
/* While the AEC calibration sweep owns the I2S channels the capture loop must
 * keep its hands off them. */
static volatile bool s_calibrating;
/* Set by the "enroll <name>" console command: the next
 * CONFIG_VOICE_ENROLL_SENTENCES utterances are averaged into that person's
 * voiceprint instead of becoming normal turns ("enroll cancel" aborts). */
static char s_enroll_name[SPEAKER_NAME_LEN];
/* Diagnostic switch: "asr off" skips the cloud transcription but still records
 * the WAV and runs the voiceprint.  Used for clean voiceprint captures (the
 * cloud ASR otherwise blocks the utterance worker when the network hiccups). */
static volatile bool s_asr_disabled;


static void *psram(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(bytes);
    }
    return p;
}

/* Every capture block is stored twice: the AFE output (for ASR / WAV) and the
 * raw microphone signal (for the voiceprint).  The two rings share one head /
 * count because they are fed the same number of blocks. */
static void preroll_push(const int16_t *frame, size_t n, const int16_t *raw, size_t rn)
{
    if (n > s_frame) {
        n = s_frame;   /* never write past a slot */
    }
    if (rn > s_frame) {
        rn = s_frame;
    }
    memcpy(&s_preroll[s_preroll_head * s_frame], frame, n * sizeof(int16_t));
    if (n < s_frame) {
        memset(&s_preroll[s_preroll_head * s_frame + n], 0,
               (s_frame - n) * sizeof(int16_t));
    }
    memcpy(&s_preroll_raw[s_preroll_head * s_frame], raw, rn * sizeof(int16_t));
    if (rn < s_frame) {
        memset(&s_preroll_raw[s_preroll_head * s_frame + rn], 0,
               (s_frame - rn) * sizeof(int16_t));
    }
    s_preroll_head = (s_preroll_head + 1) % PREROLL_FRAMES;
    if (s_preroll_count < PREROLL_FRAMES) {
        s_preroll_count++;
    }
}

static void utt_append(const int16_t *src, size_t n, const int16_t *raw, size_t rn)
{
    if (s_utt_len + n > UTT_MAX_SAMPLES) {
        n = UTT_MAX_SAMPLES - s_utt_len;
    }
    if (n) {
        memcpy(&s_utt[s_utt_len], src, n * sizeof(int16_t));
        s_utt_len += n;
    }
    if (s_utt_raw_len + rn > UTT_MAX_SAMPLES) {
        rn = UTT_MAX_SAMPLES - s_utt_raw_len;
    }
    if (rn) {
        memcpy(&s_utt_raw[s_utt_raw_len], raw, rn * sizeof(int16_t));
        s_utt_raw_len += rn;
    }
}

static void utt_seed_from_preroll(void)
{
    int start = (s_preroll_head - s_preroll_count + PREROLL_FRAMES) % PREROLL_FRAMES;
    for (int i = 0; i < s_preroll_count; i++) {
        int idx = ((start + i) % PREROLL_FRAMES) * s_frame;
        utt_append(&s_preroll[idx], s_frame, &s_preroll_raw[idx], s_frame);
    }
}

#if CONFIG_VOICE_PLAYBACK
static void playback(const int16_t *mono, size_t n)
{
    enum { CH = 256 };
    int16_t buf[CH * AUDIO_CHANNELS];
    size_t done = 0;
    while (done < n) {
        size_t k = n - done;
        if (k > CH) {
            k = CH;
        }
        for (size_t i = 0; i < k; i++) {
            buf[i * AUDIO_CHANNELS] = mono[done + i];
            buf[i * AUDIO_CHANNELS + 1] = mono[done + i];
        }
        if (audio_write(buf, k * AUDIO_CHANNELS * sizeof(int16_t)) != ESP_OK) {
            break;
        }
        done += k;
    }
}
#endif

static void process_utterance(const int16_t *pcm, size_t pcm_len,
                              const int16_t *raw, size_t raw_len, int peak_rms)
{
    if (pcm_len < (size_t)(AUDIO_SAMPLE_RATE / 4)) {
        pcm_len = 0;
        return;
    }
    uint32_t dur_ms = (uint32_t)((pcm_len * 1000ULL) / AUDIO_SAMPLE_RATE);
    const char *face = identity_face_id();
    const char *speaker = identity_speaker_id();
    char rel[128] = { 0 };

    if (storage_save_wav(pcm, pcm_len, AUDIO_SAMPLE_RATE, dur_ms, rel, sizeof(rel)) != ESP_OK) {
        ESP_LOGE(TAG, "failed to store utterance (%u samples)", (unsigned)pcm_len);
        pcm_len = 0;
        return;
    }


    /* Voiceprint source: by default the PRE-AFE microphone signal, because the
     * AFE's AEC/NS/AGC reshape the spectrum and flatten speaker timbre (ASR
     * keeps using the echo-cancelled stream above).  CONFIG_VOICE_VP_INPUT_RAW=n
     * falls back to the old AFE-input behaviour for A/B tests. */
    const int16_t *vp_pcm = pcm;
    size_t vp_len = pcm_len;
#if CONFIG_VOICE_VP_INPUT_RAW
    if (raw && raw_len >= (size_t)(AUDIO_SAMPLE_RATE / 4)) {
        vp_pcm = raw;
        vp_len = raw_len;
    }
#endif
    /* The voiceprint feature dump (VPVEC) must never mix domains: log which
     * input actually fed the extractor so the calibration parser can skip
     * fallback samples. */
    ESP_LOGI(TAG, "vp source: %s (%u samples)",
             (vp_pcm == pcm) ? "afe" : "raw", (unsigned)vp_len);

    if (s_enroll_name[0]) {
        /* Enrolment turns: collect several utterances and average them; do not
         * talk to the API while doing so. */
        esp_err_t e = speaker_id_enroll_add(vp_pcm, vp_len);
        if (e == ESP_OK) {
            int got = speaker_id_enroll_progress();
            if (got >= CONFIG_VOICE_ENROLL_SENTENCES) {
                char name[SPEAKER_NAME_LEN];
                snprintf(name, sizeof(name), "%s", s_enroll_name);
                if (speaker_id_enroll_commit() == ESP_OK) {
                    printf("* 已登记声纹：%s（%d 句平均）\n", name, got);
                } else {
                    printf("* 声纹登记失败（内存不足或角色表已满）\n");
                }
                s_enroll_name[0] = 0;
            } else {
                printf("* 已采集 %d/%d 句，请再说一句…\n",
                       got, CONFIG_VOICE_ENROLL_SENTENCES);
            }
        } else {
            printf("* 这一句不可用（%s），请再说一句（已有 %d/%d）\n",
                   esp_err_to_name(e), speaker_id_enroll_progress(),
                   CONFIG_VOICE_ENROLL_SENTENCES);
        }
        pcm_len = 0;
        return;
    }

    /* Who is speaking?  Decided on the board by esp-dl, no cloud involved. */
    /* Enrolled voices keep their name; everyone else gets a stable 人N label so
     * a multi-person conversation is still attributable. */
    const char *spk = speaker_id_identify_or_cluster(vp_pcm, vp_len);
    identity_set_last_speaker(spk);

    /* ASR is the one stage the P4 cannot host, so the clip goes out over the
     * on-board C6 Wi-Fi and comes back as text.  The transcript then feeds
     * DeepSeek; both results land in the same JSONL record as the audio. */
    char text[768] = { 0 };
    char reply[3072] = { 0 };

    esp_err_t asr_err = ESP_FAIL;
    if (asr_configured() && !s_asr_disabled) {
        int64_t asr_t0 = esp_timer_get_time();
        asr_err = asr_transcribe(pcm, pcm_len, AUDIO_SAMPLE_RATE, text, sizeof(text));
        int64_t asr_ms = (esp_timer_get_time() - asr_t0) / 1000;
        /* Retry only a fast failure.  A timeout already means the network is
         * sick; retrying it would hold the utterance worker (and the only TLS
         * lock) for another 20 s while the capture queue overflows - the board
         * looks dead and drops the next sentences. */
        if (asr_err != ESP_OK && asr_ms < 5000) {
            ESP_LOGW(TAG, "ASR failed in %lld ms (%s), retrying once",
                     (long long)asr_ms, esp_err_to_name(asr_err));
            vTaskDelay(pdMS_TO_TICKS(800));
            asr_err = asr_transcribe(pcm, pcm_len, AUDIO_SAMPLE_RATE, text, sizeof(text));
        } else if (asr_err != ESP_OK) {
            ESP_LOGW(TAG, "ASR failed after %lld ms (%s), not retrying",
                     (long long)asr_ms, esp_err_to_name(asr_err));
        }
    }

    if (asr_err == ESP_OK && text[0]) {
        ESP_LOGI(TAG, "heard: %s", text);
        /* Every utterance goes into the rolling transcript, whether or not it
         * deserves an answer - that is what the supervisor listens to. */
        conversation_add(spk, text);
    }

    if (asr_err == ESP_OK && text[0] && conversation_should_reply(text)) {
        ESP_LOGI(TAG, "reply path");
        /* Streaming: the model's sentences are queued to TTS while it is still
         * generating, so the board starts talking almost immediately instead of
         * after the whole paragraph.  'spoken' says whether that already
         * happened and the text must not be queued a second time. */
        bool spoken = false;
        if (deepseek_ask_stream(text, reply, sizeof(reply), &spoken) == ESP_OK) {
            if (!reply[0]) {
                /* The API can legitimately return an empty message (content
                 * filtered, or only reasoning tokens).  Never speak silence. */
                ESP_LOGW(TAG, "DeepSeek returned an empty reply");
                snprintf(reply, sizeof(reply), "抱歉，我没想好怎么回答。");
            }
            ESP_LOGI(TAG, "reply: %s%s", reply, spoken ? "   [streamed]" : "");
            /* Remember what was just said so the supervisor does not repeat the
             * same correction a few seconds later. */
            conversation_note_reply(reply);
            if (!spoken && tts_ready()) {
                /* Fallback text (or TTS was not ready mid-stream): say it now. */
                tts_say(reply);
                s_suppress_until_us = esp_timer_get_time() +
                                      (int64_t)(tts_last_clip_ms() + 800) * 1000;
                if (!aec_ready()) {
                    s_suppress_until_us = esp_timer_get_time() +
                                          (int64_t)(tts_last_clip_ms() + 1000) * 1000;
                }
            } else if (spoken) {
                /* Clips are already queued; tts_speaking() gates the mic for the
                 * whole playback, this only covers the ring-out after the last
                 * one. */
                s_suppress_until_us = esp_timer_get_time() + 1500 * 1000;
                if (!aec_ready()) {
                }
            }
        } else {
            ESP_LOGW(TAG, "DeepSeek failed");
        }
    } else if (asr_err == ESP_OK && text[0]) {
        /* Heard and recorded, but the current mode does not answer it
         * (HYBRID/MONITOR).  Logging "no transcript" here used to send every
         * debugging session down the wrong path. */
        ESP_LOGI(TAG, "recorded only (mode %s): not addressed",
                 conversation_mode_name(conversation_mode()));
    } else {
        ESP_LOGW(TAG, "no transcript (ASR key set? network up?)");
    }


    storage_append_turn(rel, dur_ms, peak_rms, face, speaker, text, reply);
    ESP_LOGI(TAG, "utterance #%u  %.1f s  peak_rms=%d  face=%s  speaker=%s  -> %s",
             ++s_utterance_no, dur_ms / 1000.0, peak_rms, face, speaker, rel);

    /* The SD write blocks the capture loop for a while; drop whatever the I2S
     * RX ring collected meanwhile instead of treating it as speech. */

#if CONFIG_VOICE_PLAYBACK
    /* Play what was just captured - proves mic -> codec -> amplifier end to end. */
    playback(pcm, pcm_len);
#endif
    pcm_len = 0;
}

/* ---------------- capture -> utterance worker hand-off ------------------ */
/* ASR is a cloud round trip (1-3 s) and used to run on the capture loop, so
 * 1-3 s of conversation after every utterance was simply never recorded.  Now
 * the capture loop only copies the clip and drops it in a queue; the worker
 * does the slow work (WAV + voiceprint + ASR + LLM + TTS) while the microphone
 * keeps listening.  This is what makes fast multi-person conversation work. */
#define UTT_QUEUE_DEPTH 6

typedef struct {
    int16_t *pcm;       /* AFE output: ASR + WAV */
    size_t   len;
    int16_t *raw;       /* pre-AFE microphone signal: voiceprint */
    size_t   raw_len;
    int      peak_rms;
} utt_job_t;

static QueueHandle_t s_utt_q;
static volatile int s_utt_dropped;

static void enqueue_utterance(void)
{
    if (s_utt_len < (size_t)(AUDIO_SAMPLE_RATE / 4)) {
        s_utt_len = 0;      /* too short to be speech */
        s_utt_raw_len = 0;
        return;
    }
    if (!s_utt_q) {
        s_utt_len = 0;
        return;
    }
    int16_t *copy = heap_caps_malloc(s_utt_len * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        ESP_LOGW(TAG, "no PSRAM for utterance, dropped");
        s_utt_len = 0;
        s_utt_raw_len = 0;
        return;
    }
    memcpy(copy, s_utt, s_utt_len * sizeof(int16_t));
    int16_t *copy_raw = NULL;
    if (s_utt_raw_len >= (size_t)(AUDIO_SAMPLE_RATE / 4)) {
        copy_raw = heap_caps_malloc(s_utt_raw_len * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (copy_raw) {
            memcpy(copy_raw, s_utt_raw, s_utt_raw_len * sizeof(int16_t));
        }
    }
    utt_job_t job = { .pcm = copy, .len = s_utt_len, .raw = copy_raw,
                      .raw_len = copy_raw ? s_utt_raw_len : 0,
                      .peak_rms = s_rms_peak };
    s_utt_len = 0;
    s_utt_raw_len = 0;

    if (xQueueSend(s_utt_q, &job, 0) != pdTRUE) {
        /* Worker is behind: drop the OLDEST clip, keep the newest - a live
         * conversation matters more than a backlog. */
        utt_job_t old;
        if (xQueueReceive(s_utt_q, &old, 0) == pdTRUE) {
            free(old.pcm);
            free(old.raw);
            s_utt_dropped++;
            ESP_LOGW(TAG, "capture queue full: dropped oldest clip (%d dropped total)",
                     s_utt_dropped);
        }
        if (xQueueSend(s_utt_q, &job, 0) != pdTRUE) {
            free(copy);
            free(copy_raw);
            s_utt_dropped++;
            ESP_LOGW(TAG, "capture queue full: dropped newest clip (%d dropped total)",
                     s_utt_dropped);
        }
    }
    ESP_LOGI(TAG, "queued utterance %u samples (queue %u/%d)", (unsigned)job.len,
             (unsigned)uxQueueMessagesWaiting(s_utt_q), UTT_QUEUE_DEPTH);
}

/* Per-turn memory snapshot.  Internal RAM is the scarce resource on this board
 * (one TLS session needs DMA-capable internal buffers), so every turn logs how
 * much it consumed and warns when the watermark gets dangerous.  PSRAM has
 * megabytes to spare; internal free/largest is what predicts -0x008D. */
static void log_mem_snapshot(const char *phase, size_t internal_before, size_t psram_before)
{
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    UBaseType_t stack_hwm = uxTaskGetStackHighWaterMark(NULL);
    if (internal_before || psram_before) {
        ESP_LOGI(TAG, "mem %s: internal %u->%u B (largest %u), psram %u->%u B (largest %u), stack hwm %u",
                 phase, (unsigned)internal_before, (unsigned)internal_free,
                 (unsigned)internal_largest, (unsigned)psram_before, (unsigned)psram_free,
                 (unsigned)psram_largest, (unsigned)stack_hwm);
    } else {
        ESP_LOGI(TAG, "mem %s: internal free %u B (largest %u), psram free %u B (largest %u), stack hwm %u",
                 phase, (unsigned)internal_free, (unsigned)internal_largest,
                 (unsigned)psram_free, (unsigned)psram_largest, (unsigned)stack_hwm);
    }
    if (internal_free < 20000 || internal_largest < 8192) {
        ESP_LOGW(TAG, "LOW internal RAM after %s: free %u B, largest %u B (next TLS may fail)",
                 phase, (unsigned)internal_free, (unsigned)internal_largest);
    }
}

static void utterance_worker(void *arg)
{
    (void)arg;
    utt_job_t job;
    while (1) {
        if (xQueueReceive(s_utt_q, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        /* Tell the supervisor a reply turn is running: it must not grab the
         * single TLS lock while the user is waiting for an answer. */
        conversation_note_turn_start();
        /* Only one cloud round trip at a time (see conversation_lock_cloud). */
        conversation_lock_cloud();
        process_utterance(job.pcm, job.len, job.raw, job.raw_len, job.peak_rms);
        conversation_unlock_cloud();
        conversation_note_turn_end();
        /* Snapshot before releasing the clip so 'before' and 'after' measure the
         * same set of buffers. */
        log_mem_snapshot("utterance turn", internal_before, psram_before);
        free(job.pcm);
        free(job.raw);
    }
}

/* One mono frame's worth of VAD bookkeeping, shared by the plain I2S path and
 * the AEC path so the two cannot drift apart. */
static void feed_vad_frame(vad_t *vad, const int16_t *mono, size_t n,
                           const int16_t *raw, size_t raw_n, int64_t *last_report_us)
{
    bool was = vad_in_speech(vad);
    int rms = 0;
    vad_event_t ev = vad_feed(vad, mono, n, &rms);
    bool now = vad_in_speech(vad);


    if (was) {
        utt_append(mono, n, raw, raw_n);
        if (rms > s_rms_peak) {
            s_rms_peak = rms;
        }
    } else if (now) {
        s_utt_len = 0;
        s_utt_raw_len = 0;
        s_rms_peak = rms;
        utt_seed_from_preroll();
        utt_append(mono, n, raw, raw_n);
        ESP_LOGI(TAG, "speech detected (rms=%d, gate=%d)", rms, vad_threshold(vad));
    } else {
        preroll_push(mono, n, raw, raw_n);
        int64_t now_us = esp_timer_get_time();
        if (now_us - *last_report_us > 5000000) {
            *last_report_us = now_us;
            ESP_LOGI(TAG, "idle  rms=%d  (open above %d)  wifi=%s  sd=%s",
                     rms, CONFIG_VOICE_VAD_START_RMS,
                     wifi_connected() ? "up" : "down",
                     storage_ready() ? "ok" : "missing");
        }
    }

    if (ev == VAD_EVENT_UTTERANCE_DONE) {
        enqueue_utterance();
    }
}

#if CONFIG_VOICE_AEC
/* Read exactly one AFE feed block, pair every microphone sample with the
 * matching playback sample, and hand the pair to the echo canceller. */
static const int16_t *aec_next_block(int *out_n, const int16_t **raw_out, int *raw_n)
{
    enum { MAX_FS = 1024 };
    static int16_t stereo[MAX_FS * AUDIO_CHANNELS];
    static int16_t mic[MAX_FS];
    static int16_t ref[MAX_FS];
    static int16_t inter[MAX_FS * 2];

    int fs = aec_feed_chunksize();
    if (fs <= 0 || fs > MAX_FS) {
        return NULL;
    }
    size_t need = (size_t)fs * AUDIO_CHANNELS * sizeof(int16_t);
    size_t off = 0;
    while (off < need) {
        size_t got = 0;
        if (audio_read((int16_t *)((uint8_t *)stereo + off), need - off, &got, 200) != ESP_OK || got == 0) {
            return NULL;
        }
        off += got;
    }

    audio_downmix(stereo, (size_t)fs * AUDIO_CHANNELS, mic);
    /* Keep the untouched microphone block for the voiceprint branch. */
    if (raw_out) {
        *raw_out = mic;
    }
    if (raw_n) {
        *raw_n = fs;
    }
    /* Interleave properly: the AFE expects [mic0, ref0, mic1, ref1, ...].  The
     * old code popped the reference contiguously into &inter[1] and then
     * overwrote every even slot with the microphone, so the canceller only saw
     * ref[0], ref[2], ... (plus a block of zeros) - it never got a usable
     * reference at all.  That is why AEC could not cancel the speaker and the
     * microphone has to be gated during playback. */
    audio_ref_pop(ref, fs);
    for (int i = 0; i < fs; i++) {
        inter[i * 2] = mic[i];             /* even slots = microphone */
        inter[i * 2 + 1] = ref[i];         /* odd slots = playback reference */
    }
    return app_aec_process(inter, out_n);
}
#endif

/* The UART picks up a stray byte or two while the board resets; that is not
 * text and must never reach the API (DeepSeek rejects invalid UTF-8 with 400). */
static bool utf8_is_text(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x20) {
            return false;
        }
        int extra;
        if (*p < 0x80) {
            extra = 0;
        } else if ((*p & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((*p & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((*p & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            return false;
        }
        p++;
        for (int i = 0; i < extra; i++, p++) {
            if ((*p & 0xC0) != 0x80) {
                return false;
            }
        }
    }
    return p != (const unsigned char *)s;
}

/* Text path: proves DeepSeek + the SD log end to end before any ASR exists.
 * Once ESP-SR TTS is wired in, the reply is spoken here as well. */
/* --------------------- AEC reference delay calibration ------------------- */
/* The canceller is fed the playback reference the moment audio_write() is
 * called, but the sound reaches the microphone only after the I2S DMA drains
 * and the codec clocks it out, so the reference arrives too early to cancel
 * anything.  This measures that delay directly:
 *   1. play 500 ms of pseudo-random noise,
 *   2. record the microphone while it plays,
 *   3. cross-correlate the recording with the noise,
 *   4. the lag of the peak IS the play->capture sample delay.
 * The reference path is then held back by that many samples
 * (audio_ref_set_delay) and the value is stored in NVS. */
#define CAL_LEN        8000     /* 500 ms of PRN at 16 kHz */
#define CAL_MAX_LAG    3200     /* search up to 200 ms */
#define CAL_READ_CHUNK 512
#define CAL_DECIMATE   4        /* correlation step, keeps it fast with floats */

static void aec_delay_save(int samples)
{
    /* app_main reaches aec_init() before wifi_connect() calls nvs_flash_init(),
     * so make sure NVS is up before touching it.  Idempotent. */
    static bool nvs_ready;
    if (!nvs_ready) {
        nvs_flash_init();
        nvs_ready = true;
    }
    nvs_handle_t h;
    if (nvs_open("audio", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "aec_dly", samples);
        nvs_commit(h);
        nvs_close(h);
    }
}

static int aec_delay_load(void)
{
    static bool nvs_ready;
    if (!nvs_ready) {
        nvs_flash_init();       /* not called yet at this point in app_main */
        nvs_ready = true;
    }
    nvs_handle_t h;
    int32_t v = -1;
    if (nvs_open("audio", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, "aec_dly", &v) != ESP_OK) {
            v = -1;
        }
        nvs_close(h);
    }
    return (int)v;
}

static void calib_player_task(void *arg)
{
    int16_t *play = (int16_t *)arg;
    audio_write(play, CAL_LEN * AUDIO_CHANNELS * sizeof(int16_t));
    vTaskDelete(NULL);
}

static void run_aec_calibration(void)
{
    int16_t *prn = psram(CAL_LEN * sizeof(int16_t));
    int16_t *play = psram(CAL_LEN * AUDIO_CHANNELS * sizeof(int16_t));
    int16_t *mic_st = psram(CAL_READ_CHUNK * AUDIO_CHANNELS * sizeof(int16_t));
    int16_t *mic = psram((CAL_LEN + CAL_MAX_LAG) * sizeof(int16_t));
    if (!prn || !play || !mic_st || !mic) {
        printf("!! 内存不足\n");
        free(prn); free(play); free(mic_st); free(mic);
        return;
    }

    uint32_t st = 0x12345678u;
    for (int i = 0; i < CAL_LEN; i++) {
        st ^= st << 13; st ^= st >> 17; st ^= st << 5;
        int16_t v = (int16_t)((st >> 16) / 2);
        prn[i] = v;
        play[i * AUDIO_CHANNELS] = v;
        play[i * AUDIO_CHANNELS + 1] = v;
    }

    printf("* AEC 标定：播放 500 ms 噪声并录音，互相关搜索 0..%d ms\n",
           CAL_MAX_LAG * 1000 / AUDIO_SAMPLE_RATE);

    audio_drain_rx();
    if (xTaskCreate(calib_player_task, "calib_play", 4096, play, 6, NULL) != pdPASS) {
        printf("!! 播放任务创建失败\n");
        free(prn); free(play); free(mic_st); free(mic);
        return;
    }

    size_t want = CAL_LEN + CAL_MAX_LAG;
    size_t have = 0;
    int64_t t0 = esp_timer_get_time();
    while (have < want) {
        size_t got = 0;
        if (audio_read(mic_st, CAL_READ_CHUNK * AUDIO_CHANNELS * sizeof(int16_t),
                       &got, 200) != ESP_OK || got == 0) {
            break;
        }
        size_t stereo = got / sizeof(int16_t);
        int n = (int)(stereo / AUDIO_CHANNELS);
        if (have + (size_t)n > want) {
            n = (int)(want - have);
        }
        audio_downmix(mic_st, (size_t)n * AUDIO_CHANNELS, mic + have);
        have += (size_t)n;
    }
    ESP_LOGI(TAG, "calib captured %u/%u samples in %lld ms", (unsigned)have,
             (unsigned)want, (esp_timer_get_time() - t0) / 1000);

    if (have < (size_t)(CAL_LEN + 800)) {
        printf("!! 录音不足（%u 样本），标定中止\n", (unsigned)have);
        free(prn); free(play); free(mic_st); free(mic);
        return;
    }

    int search = (int)have - CAL_LEN;
    if (search > CAL_MAX_LAG) {
        search = CAL_MAX_LAG;
    }
    float prn2 = 0.0f, mic2 = 0.0f;
    for (int i = 0; i < CAL_LEN; i += CAL_DECIMATE) {
        prn2 += (float)prn[i] * (float)prn[i];
        mic2 += (float)mic[i] * (float)mic[i];
    }
    float best = 0.0f;
    int best_lag = 0;
    for (int lag = 0; lag <= search; lag++) {
        float s = 0.0f;
        for (int i = 0; i < CAL_LEN; i += CAL_DECIMATE) {
            s += (float)prn[i] * (float)mic[i + lag];
        }
        if (fabsf(s) > fabsf(best)) {
            best = s;
            best_lag = lag;
        }
    }
    float norm = sqrtf(prn2 * mic2);
    float coeff = norm > 1.0f ? fabsf(best) / norm : 0.0f;
    printf("* 互相关峰值：延迟 %d 样本 (%d ms)，相关系数 %.3f\n",
           best_lag, best_lag * 1000 / AUDIO_SAMPLE_RATE, coeff);
    if (coeff < 0.05f) {
        printf("!! 相关性偏低：麦克风可能几乎收不到喇叭声，参考延迟仍需保留旧值\n");
    }
    audio_ref_set_delay(best_lag);
    aec_delay_save(best_lag);
    printf("* AEC 参考延迟 = %d 样本 (%d ms)，已写入 NVS\n",
           best_lag, best_lag * 1000 / AUDIO_SAMPLE_RATE);

    free(prn); free(play); free(mic_st); free(mic);
}

static void on_console_line(const char *line)
{
    if (!utf8_is_text(line)) {
        ESP_LOGW(TAG, "ignoring non-text console input (%u bytes)", (unsigned)strlen(line));
        return;
    }
    if (strcmp(line, "dump") == 0) {
        storage_dump_index(8);
        return;
    }
    if (strcmp(line, "wavtest") == 0) {
        /* Exercises the whole recording path without needing anyone to speak. */
        enum { N = 16000 };
        static int16_t tone[N];
        for (int i = 0; i < N; i++) {
            tone[i] = (int16_t)(3000 * sinf(2.0f * (float)M_PI * 440.0f * i / AUDIO_SAMPLE_RATE));
        }
        char rel[128] = { 0 };
        esp_err_t e = storage_save_wav(tone, N, AUDIO_SAMPLE_RATE, 1000, rel, sizeof(rel));
        printf("storage_save_wav -> %s  %s\n", esp_err_to_name(e), e == ESP_OK ? rel : "（失败）");
        if (e == ESP_OK) {
            e = storage_append_turn(rel, 1000, 3000, "unknown", "test", "wavtest", "自检录音");
            printf("storage_append_turn -> %s\n", esp_err_to_name(e));
        }
        return;
    }
    if (strcmp(line, "df") == 0) {
        /* statvfs is ENOSYS on the FAT VFS; esp_vfs_fat_info is the API that
         * actually works here. */
        uint64_t total = 0, freeb = 0;
        if (!storage_ready()) {
            printf("TF 卡未挂载\n");
        } else if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &freeb) != ESP_OK) {
            printf("取不到卡容量信息\n");
        } else {
            printf("TF 卡: 共 %llu MB，可用 %llu MB\n",
                   (unsigned long long)(total >> 20), (unsigned long long)(freeb >> 20));
        }
        return;
    }
    if (strcmp(line, "wifi") == 0) {
        char ssid[33] = { 0 };
        char ip[20] = { 0 };
        int rssi = 0;
        bool link = wifi_link_info(ssid, sizeof(ssid), ip, sizeof(ip), &rssi);
        printf("* 当前连接 : %s\n", link ? ssid : "(未连接)");
        printf("* IP       : %s\n", link ? ip : "-");
        printf("* 信号     : %d dBm\n", rssi);
        printf("* 配置 SSID: %s（NVS 优先，其次 menuconfig）\n", wifi_current_ssid());
        return;
    }
    if (strcmp(line, "ap") == 0) {
        printf("* 进入配网模式，最多等待 180 秒\n");
        if (wifi_start_provisioning(180000) == ESP_OK) {
            char ip[20] = { 0 };
            wifi_link_info(NULL, 0, ip, sizeof(ip), NULL);
            printf("* 配网成功：%s，IP %s\n", wifi_current_ssid(), ip);
        } else {
            printf("!! 配网失败或超时\n");
        }
        return;
    }
    if (strncmp(line, "wifi ", 5) == 0 && line[5]) {
        char ssid[33] = { 0 };
        char pass[65] = { 0 };
        const char *rest = line + 5;
        /* The password is the LAST token, the SSID is everything before it, so
         * an SSID containing spaces still works.  A single token means an open
         * network. */
        const char *last = strrchr(rest, ' ');
        if (last) {
            size_t n = (size_t)(last - rest);
            if (n >= sizeof(ssid)) {
                n = sizeof(ssid) - 1;
            }
            memcpy(ssid, rest, n);
            ssid[n] = 0;
            snprintf(pass, sizeof(pass), "%s", last + 1);
        } else {
            snprintf(ssid, sizeof(ssid), "%s", rest);
        }
        printf("* 连接 %s（凭据将保存到 NVS）…\n", ssid);
        if (wifi_join(ssid, pass, 25000) == ESP_OK) {
            char ip[20] = { 0 };
            wifi_link_info(NULL, 0, ip, sizeof(ip), NULL);
            printf("* 已连接，IP %s\n", ip);
        } else {
            printf("!! 连接失败（密码错？信号弱？）\n");
        }
        return;
    }
    if (strncmp(line, "micgain ", 8) == 0) {
        int db = atoi(line + 8);
        audio_set_mic_gain_db(db);
        printf("* 麦克风增益设为 %d dB（重启恢复）\n", db);
        return;
    }
    if (strncmp(line, "ask ", 4) == 0 && line[4]) {
        /* Same path as a spoken question (streaming + TTS), but triggered from
         * the console so the latency can be measured without a microphone. */
        char *reply = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!reply) {
            reply = malloc(4096);
        }
        if (!reply) {
            printf("!! out of memory\n");
            return;
        }
        bool spoken = false;
        size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        printf("* 提问：%s\n", line + 4);
        /* Same serialisation the utterance worker uses: a console question must
         * never open a second mbedTLS session while ASR/another turn is up. */
        conversation_note_turn_start();
        conversation_lock_cloud();
        esp_err_t ask_err = deepseek_ask_stream(line + 4, reply, 4096, &spoken);
        conversation_unlock_cloud();
        conversation_note_turn_end();
        if (ask_err == ESP_OK) {
            conversation_note_reply(reply);
            printf("<< %s%s\n", reply, spoken ? "   [已流式朗读]" : "");
        } else {
            printf("!! DeepSeek 失败\n");
        }
        log_mem_snapshot("console ask", internal_before, psram_before);
        free(reply);
        return;
    }
    if (strncmp(line, "say ", 4) == 0 && line[4]) {
        /* Speak arbitrary text: proves the TTS -> ES8311 -> NS4150B path
         * independently of the microphone.  Uses the currently selected engine,
         * so "tts local" then "say ..." is a direct A/B of the two voices. */
        if (!tts_ready()) {
            printf("!! TTS 未就绪\n");
            return;
        }
        printf("* 朗读（%s）：%s\n", tts_engine_name(), line + 4);
        tts_say(line + 4);
        return;
    }
    if (strcmp(line, "tts") == 0) {
        printf("* 引擎: %s（auto=有网走云端，没网走板载）\n", tts_engine_name());
        if (tts_cloud_configured()) {
            printf("* 云端: %s  音色 %s  语速 %d%%  增益 %d dB\n",
                   tts_cloud_active() ? "可用" :
                   (wifi_connected() ? "已配置（当前已关闭）" : "已配置（Wi-Fi 未连接）"),
                   tts_cloud_voice(), CONFIG_TTS_SPEED_X100, CONFIG_TTS_GAIN_DB);
        } else {
            printf("* 云端: 未配置（没有 SiliconFlow key）\n");
        }
        printf("* 板载 ESP-SR: %s，语速 %u\n", tts_ready() ? "就绪" : "不可用",
               tts_local_speed());
        printf("* 用法: tts auto | tts cloud | tts local | tts voice <音色>\n");
        return;
    }
    if (strncmp(line, "tts ", 4) == 0) {
        const char *mode = line + 4;
        if (strcmp(mode, "cloud") == 0) {
            tts_set_engine(TTS_ENGINE_CLOUD);
            printf("* TTS 引擎：cloud（云端优先，失败自动回退板载）\n");
        } else if (strcmp(mode, "local") == 0) {
            tts_set_engine(TTS_ENGINE_LOCAL);
            printf("* TTS 引擎：local（只用板载 ESP-SR）\n");
        } else if (strcmp(mode, "auto") == 0) {
            tts_set_engine(TTS_ENGINE_AUTO);
            printf("* TTS 引擎：auto（有网云端，没网板载）\n");
        } else if (strncmp(mode, "voice ", 6) == 0 && mode[6]) {
            tts_cloud_set_voice(mode + 6);
            printf("* 云端音色：%s\n", tts_cloud_voice());
            printf("* 女声 anna/bella/claire/diana，男声 alex/benjamin/charles/david\n");
            printf("* 试听: say 你好呀，我在呢\n");
        } else {
            printf("* 用法: tts auto | tts cloud | tts local | tts voice <音色>\n");
        }
        return;
    }
    if (strncmp(line, "recent", 6) == 0) {
        /* Show what the board actually heard and answered, straight from the
         * JSONL log on the TF card.  This is the evidence for "did it hear me
         * but stay silent" vs "it never heard me at all". */
        int want = 3;
        if (line[6] == ' ' && line[7]) {
            want = atoi(line + 7);
        }
        if (want < 1) {
            want = 1;
        }
        if (want > 10) {
            want = 10;
        }
        char *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            buf = malloc(4096);
        }
        if (!buf) {
            printf("!! out of memory\n");
            return;
        }
        cJSON *a = cJSON_CreateObject();
        cJSON_AddNumberToObject(a, "count", want);
        if (!tools_execute("recent_conversations", a, buf, 4096)) {
            printf("!! 读取失败\n");
        } else {
            printf("%s\n", buf);
        }
        cJSON_Delete(a);
        free(buf);
        return;
    }
    if (strncmp(line, "vpvec", 5) == 0 && (line[5] == 0 || line[5] == ' ')) {
        if (line[5] == ' ' && line[6]) {
            if (strcmp(line + 6, "on") == 0) {
                speaker_id_set_dump_raw(true);
            } else if (strcmp(line + 6, "off") == 0) {
                speaker_id_set_dump_raw(false);
            } else {
                printf("* 用法：vpvec [on|off]（打印 43 维未标准化特征，用于重标定表）\n");
                return;
            }
        }
        printf("* VPVEC 原始特征输出：%s\n", speaker_id_dump_raw() ? "开" : "关");
        return;
    }
    if (strncmp(line, "cluster", 7) == 0) {
        if (strcmp(line, "cluster reset") == 0) {
            speaker_id_reset_clusters();
            printf("* 匿名说话人聚类已重置（下一个人从 人1 开始）\n");
        } else {
            char *buf = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!buf) {
                buf = malloc(1024);
            }
            if (!buf) {
                printf("!! out of memory\n");
                return;
            }
            speaker_id_info(buf, 1024);
            printf("%s", buf);
            printf("* 用法：cluster reset（清除自动创建的角色）\n");
            free(buf);
        }
        return;
    }
    if (strncmp(line, "city", 4) == 0) {
        if (line[4] == ' ' && line[5]) {
            conversation_set_city(line + 5);
            printf("* 城市已设为 %s（用于天气等事实核查）\n", conversation_city());
        } else {
            printf("* 当前城市：%s\n", conversation_city());
        }
        return;
    }
    if (strncmp(line, "conv say ", 9) == 0 && line[9]) {
        conversation_add("主人", line + 9);
        printf("* 已加入对话记录（可用 'conv eval' 让模型判断是否插话）\n");
        return;
    }
    if (strcmp(line, "conv eval") == 0) {
        printf("* 正在判断…\n");
        char say[256];
        int verdict = conversation_eval_once(true, say, sizeof(say));
        if (verdict > 0) {
            printf("* 判断结果：插话 -> %s\n", say);
        } else if (verdict == 0) {
            printf("* 判断结果：不插话\n");
        } else {
            printf("* 判断失败（网络/内存），稍后可重试\n");
        }
        return;
    }
    if (strcmp(line, "conv") == 0) {
        char *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            buf = malloc(4096);
        }
        if (!buf) {
            printf("!! out of memory\n");
            return;
        }
        conversation_snapshot(buf, 4096);
        printf("%s\n", buf[0] ? buf : "(还没有对话记录)");
        free(buf);
        return;
    }
    if (strncmp(line, "mode", 4) == 0) {
        if (line[4] == ' ' && line[5]) {
            const char *m = line + 5;
            if (strcmp(m, "assistant") == 0) {
                conversation_set_mode(CONV_MODE_ASSISTANT);
            } else if (strcmp(m, "monitor") == 0) {
                conversation_set_mode(CONV_MODE_MONITOR);
            } else if (strcmp(m, "hybrid") == 0) {
                conversation_set_mode(CONV_MODE_HYBRID);
            } else {
                printf("* 用法：mode assistant|monitor|hybrid\n");
                return;
            }
            printf("* 模式已切换为 %s\n", conversation_mode_name(conversation_mode()));
        } else {
            printf("* 当前模式：%s\n", conversation_mode_name(conversation_mode()));
        }
        return;
    }
    if (strncmp(line, "supervisor", 10) == 0 && (line[10] == 0 || line[10] == ' ')) {
        if (line[10] == ' ' && line[11]) {
            const char *v = line + 11;
            if (strcmp(v, "on") == 0) {
                conversation_enable_supervisor(true);
            } else if (strcmp(v, "off") == 0) {
                conversation_enable_supervisor(false);
            } else {
                printf("* 用法：supervisor [on|off]\n");
                return;
            }
        }
        printf("* 主动插话：%s\n", conversation_supervisor_enabled() ? "开" : "关");
        return;
    }
    if (strcmp(line, "calib") == 0 || strcmp(line, "calib show") == 0) {
        printf("* 当前 AEC 参考延迟：%d 样本 (%d ms)\n",
               audio_ref_get_delay(), audio_ref_get_delay() * 1000 / AUDIO_SAMPLE_RATE);
        if (strcmp(line, "calib") == 0) {
            s_calibrating = true;
            vTaskDelay(pdMS_TO_TICKS(150));
            run_aec_calibration();
            s_calibrating = false;
        }
        return;
    }
    if (strcmp(line, "remind clear") == 0) {
        int n = reminder_cancel_all();
        printf("* 已取消 %d 个提醒\n", n);
        return;
    }
    if (strcmp(line, "reminders") == 0) {
        char buf[512];
        reminder_list(buf, sizeof(buf));
        printf("%s\n", buf);
        return;
    }
    if (strncmp(line, "remind ", 7) == 0 && line[7]) {
        int secs = atoi(line + 7);
        const char *txt = strchr(line + 7, ' ');
        if (secs <= 0 || !txt || !txt[1]) {
            printf("* 用法：remind <秒数> <提醒内容>\n");
            return;
        }
        int id = reminder_add((uint32_t)secs, txt + 1);
        if (id > 0) {
            printf("* 提醒 #%d：%d 秒后「%s」\n", id, secs, txt + 1);
        } else {
            printf("!! 提醒表已满\n");
        }
        return;
    }
    if (strncmp(line, "search ", 7) == 0 && line[7]) {
        char *buf = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            buf = malloc(2048);
        }
        if (!buf) {
            printf("!! out of memory\n");
            return;
        }
        printf("* 联网搜索：%s\n", line + 7);
        if (tools_web_search(line + 7, buf, 2048)) {
            printf("%s\n", buf);
        } else {
            printf("!! 搜索失败（网络或 DeepSeek 接口错误）\n");
        }
        free(buf);
        return;
    }
    if (strcmp(line, "who") == 0) {
        char names[SPEAKER_MAX][SPEAKER_NAME_LEN];
        int n = speaker_id_list(names, SPEAKER_MAX);
        printf("已登记 %d 人：", n);
        for (int i = 0; i < n; i++) {
            printf("%s%s", i ? ", " : "", names[i]);
        }
        printf("\n");
        return;
    }
    if (strncmp(line, "forget ", 7) == 0) {
        if (speaker_id_forget(line + 7) == ESP_OK) {
            printf("* 已删除 %s 的声纹\n", line + 7);
        } else {
            printf("* 没找到 %s\n", line + 7);
        }
        return;
    }
    if (strncmp(line, "enroll ", 7) == 0 && line[7]) {
        if (strcmp(line + 7, "cancel") == 0) {
            speaker_id_enroll_cancel();
            s_enroll_name[0] = 0;
            printf("* 已取消声纹登记\n");
            return;
        }
        if (speaker_id_enroll_begin(line + 7) != ESP_OK) {
            printf("* 声纹模块未就绪\n");
            return;
        }
        snprintf(s_enroll_name, sizeof(s_enroll_name), "%s", line + 7);
        printf("* 请说 %d 句话（每句 1-4 秒），我会取平均记住 %s 的声音\n",
               CONFIG_VOICE_ENROLL_SENTENCES, s_enroll_name);
        printf("* 中途可发 \"enroll cancel\" 取消\n");
        return;
    }
    if (strncmp(line, "asr", 3) == 0 && (line[3] == 0 || line[3] == ' ')) {
        if (line[3] == ' ' && line[4]) {
            if (strcmp(line + 4, "on") == 0) {
                s_asr_disabled = false;
            } else if (strcmp(line + 4, "off") == 0) {
                s_asr_disabled = true;
            } else {
                printf("* 用法：asr [on|off]（off = 跳过云端转写，只录音+声纹）\n");
                return;
            }
        }
        printf("* 云端 ASR：%s\n", s_asr_disabled ? "跳过（诊断模式）" : "开");
        return;
    }
    if (strncmp(line, "vpmargin", 8) == 0 && (line[8] == 0 || line[8] == ' ')) {
        if (line[8] == ' ' && line[9]) {
            int v = atoi(line + 9);
            if (v < 0 || v > 100) {
                printf("* 用法：vpmargin [0-100]（cosine x100，0=关）\n");
                return;
            }
            speaker_id_set_margin_x100(v);
        }
        printf("* 声纹 margin：%d（x100；<0 表示用 Kconfig 默认）\n",
               speaker_id_margin_x100());
        return;
    }
    char *reply = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!reply) {
        reply = malloc(4096);
    }
    if (!reply) {
        printf("!! out of memory\n");
        return;
    }
    printf(">> %s\n", line);
    conversation_lock_cloud();
    esp_err_t text_err = deepseek_ask(line, reply, 4096);
    conversation_unlock_cloud();
    if (text_err == ESP_OK) {
        printf("<< %s\n", reply);
        storage_append_turn("", 0, 0, identity_face_id(), identity_speaker_id(), line, reply);
    } else {
        printf("!! DeepSeek request failed (key set? network up?)\n");
    }
    free(reply);
}


void app_main(void)
{
    printf("\n=== ESP32-P4-WIFI6-POE-ETH voice logger ===\n");

    /* The frame width depends on whether the AFE is in the path, so the
     * buffers are allocated after aec_init() below. */
    ESP_ERROR_CHECK(board_init());
    if (storage_mount() != ESP_OK) {
        ESP_LOGW(TAG, "continuing without the TF card - utterances will be dropped");
    }
    ESP_ERROR_CHECK(audio_init());
    board_pa_enable(true);
    board_beep();
    ESP_LOGI(TAG, "audio path up (speaker chirp played)");

    /* Start the console as early as possible.  Its 16 KB task stack has to come
     * from internal DRAM, and by the time ESP-Hosted + Wi-Fi + ESP-SR + the Opus
     * codecs are up there is not enough left - the console then silently never
     * starts, which looks like a dead board over serial. */
    console_start(on_console_line);

#if CONFIG_VOICE_AEC
    if (aec_init() == ESP_OK) {
        int fs = aec_feed_chunksize();
        if (fs > 0) {
            s_frame = (size_t)fs;
        }
        ESP_LOGI(TAG, "echo cancellation active (full duplex)");
        /* Reference delay measured by 'calib' on a previous run. */
        int d = aec_delay_load();
        if (d >= 0) {
            audio_ref_set_delay(d);
            ESP_LOGI(TAG, "AEC reference delay: %d samples (%d ms, from NVS)",
                     d, d * 1000 / AUDIO_SAMPLE_RATE);
        } else {
            ESP_LOGW(TAG, "AEC reference delay not calibrated yet - run 'calib'");
        }
    } else {
        ESP_LOGW(TAG, "AEC unavailable; falling back to the suppression window");
    }
#endif

    if (s_frame == 0) {
        s_frame = FRAME_SAMPLES;
    }
    s_stereo  = psram(s_frame * AUDIO_CHANNELS * sizeof(int16_t));
    s_mono    = psram(s_frame * sizeof(int16_t));
    s_preroll = psram(PREROLL_FRAMES * s_frame * sizeof(int16_t));
    s_utt     = psram(UTT_MAX_SAMPLES * sizeof(int16_t));
    s_preroll_raw = psram(PREROLL_FRAMES * s_frame * sizeof(int16_t));
    s_utt_raw     = psram(UTT_MAX_SAMPLES * sizeof(int16_t));
    if (!s_stereo || !s_mono || !s_preroll || !s_utt || !s_preroll_raw || !s_utt_raw) {
        ESP_LOGE(TAG, "out of memory for audio buffers");
        return;
    }
    ESP_LOGI(TAG, "frame width %u samples (%u ms)", (unsigned)s_frame,
             (unsigned)(s_frame * 1000 / AUDIO_SAMPLE_RATE));

    /* Voiceprint: 43-dim MFCC+pitch supervector, fully on-board, computed from
     * the pre-AFE microphone signal (parallel raw capture above). */
    if (speaker_id_init() == ESP_OK) {
        ESP_LOGI(TAG, "voiceprint active (raw-mic input) - use 'enroll <name>' over serial to teach it a voice");
    } else {
        ESP_LOGW(TAG, "voiceprint unavailable; speaker_id will stay unknown");
    }

    /* Conversation monitor first: it creates the cloud-call mutex that the
     * utterance worker needs. */
    conversation_init(CONFIG_VOICE_DEFAULT_MODE);

    /* Utterance worker: the capture loop must never block on ASR again. */
    s_utt_q = xQueueCreate(UTT_QUEUE_DEPTH, sizeof(utt_job_t));
    if (!s_utt_q ||
        xTaskCreate(utterance_worker, "utt_work", 16384, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "utterance worker FAILED to start - nothing will be processed");
    } else {
        ESP_LOGI(TAG, "capture decoupled from ASR (queue depth %d, worker 16 KB)",
                 UTT_QUEUE_DEPTH);
    }

    /* Proactive reminders: the board must also be able to speak first. */
    if (reminder_init() != ESP_OK) {
        ESP_LOGW(TAG, "reminder scheduler unavailable");
    }

    /* Chinese TTS runs entirely on the board (ESP-SR, voice xiaoxin). */
    if (tts_init() == ESP_OK) {
        tts_say("小新已启动，我在呢");
    } else {
        ESP_LOGW(TAG, "TTS unavailable - replies will only be logged");
    }

    if (wifi_connect(20000) == ESP_OK) {
        /* Generous window: SNTP occasionally misses a 15 s deadline on a busy
         * AP, and without it every WAV lands in conv/boot/ instead of a dated
         * folder. */
        if (wifi_time_sync(45000) == ESP_OK) {
            storage_set_time_synced(true);
            ESP_LOGI(TAG, "wall-clock timestamps enabled");
        } else {
            ESP_LOGW(TAG, "SNTP failed; WAVs will use conv/boot/ and uptime stamps");
        }
    } else {
        ESP_LOGW(TAG, "no Wi-Fi; timestamps will use uptime and SSID stays unset");
    }


    vad_t vad;
    vad_init(&vad, AUDIO_SAMPLE_RATE, CONFIG_VOICE_VAD_START_RMS, CONFIG_VOICE_VAD_HANGOVER_MS,
             CONFIG_VOICE_VAD_MIN_SPEECH_MS, CONFIG_VOICE_VAD_MAX_UTTERANCE_MS);

#if CONFIG_VOICE_BLE
    /* BLE rides the same SDIO link as Wi-Fi (the C6 is the controller), so it
     * comes up after the hosted transport is running. */
    if (ble_init() == ESP_OK) {
        ESP_LOGI(TAG, "BLE ready (C6 controller over VHCI)");
    } else {
        ESP_LOGW(TAG, "BLE unavailable; bluetooth tools will say so");
    }
#else
    ESP_LOGI(TAG, "BLE not built in (CONFIG_VOICE_BLE=n)");
#endif

    ESP_LOGI(TAG, "internal heap after init: %u free, largest %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "DeepSeek: %s (model %s)", deepseek_configured() ? "key present" : "NO KEY",
             CONFIG_DEEPSEEK_MODEL);

    ESP_LOGI(TAG, "listening... (threshold rms=%d, hangover=%d ms, max=%d ms)",
             CONFIG_VOICE_VAD_START_RMS, CONFIG_VOICE_VAD_HANGOVER_MS,
             CONFIG_VOICE_VAD_MAX_UTTERANCE_MS);

    int64_t last_report_us = 0;
    while (1) {
        if (s_calibrating) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        /* The reboot tool defers the restart so the spoken reply finishes. */
        int64_t rb = tools_pending_reboot_us();
        if (rb && esp_timer_get_time() >= rb) {
            ESP_LOGW(TAG, "rebooting as requested by the user");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }

#if CONFIG_VOICE_AEC
        if (aec_ready()) {
            int n = 0;
            int raw_n = 0;
            const int16_t *raw = NULL;
            const int16_t *clean = aec_next_block(&n, &raw, &raw_n);
            if (!clean || n <= 0) {
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            /* Do NOT listen to ourselves.
             *
             * Ideally the AEC would subtract our own voice and you could talk
             * over the top.  In practice it does not: the playback reference
             * enters the canceller when audio_write() is called, but the sound
             * only leaves the speaker after the I2S DMA drains, and that fixed
             * delay is longer than aec_filter_length covers.  The result was a
             * board that transcribed its own replies (11 s "utterances" made of
             * its own words) and answered itself forever.
             *
             * So while a reply is coming out of the speaker the microphone is
             * discarded and the VAD reset.  Barge-in needs the reference delay
             * measured and aec_filter_length raised to match; until then this
             * is the correct trade. */
            if (tts_speaking() ||
                esp_timer_get_time() < s_suppress_until_us) {
                s_preroll_count = 0;
                s_preroll_head = 0;
                s_utt_len = 0;
                s_utt_raw_len = 0;
                continue;
            }
            /* With echo cancellation running there is no need to gag the
             * microphone after a reply - that suppression window exists only
             * for the path where the board can hear its own tail. */
            memcpy(s_mono, clean, (size_t)n * sizeof(int16_t));
            feed_vad_frame(&vad, s_mono, (size_t)n,
                           raw ? raw : s_mono, (size_t)(raw ? raw_n : (int)n),
                           &last_report_us);
            continue;
        }
        ESP_LOGW(TAG, "AEC enabled but AFE unavailable; using the plain path");
#endif

        size_t got = 0;
        if (audio_read(s_stereo, FRAME_STEREO * sizeof(int16_t), &got, 200) != ESP_OK || got == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (esp_timer_get_time() < s_suppress_until_us) {
            /* still our own voice in the air - keep the I2S ring drained */
            s_preroll_count = 0;
            s_preroll_head = 0;
            s_utt_len = 0;
            s_utt_raw_len = 0;
            continue;
        }

        size_t stereo_samples = got / sizeof(int16_t);
        size_t n = stereo_samples / AUDIO_CHANNELS;
        audio_downmix(s_stereo, stereo_samples, s_mono);
        feed_vad_frame(&vad, s_mono, n, s_mono, n, &last_report_us);
    }
}
