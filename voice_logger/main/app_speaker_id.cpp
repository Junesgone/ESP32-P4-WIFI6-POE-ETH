/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#include "app_speaker_id.h"

#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <vector>

#include "app_vp_tables.h"
#include "board_config.h"
#include "dl_fbank.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"

/* Lightweight on-board voiceprint.
 *
 * Feature: 43 dims built on top of esp-dl's Fbank -
 *   20 MFCC mean + 20 MFCC std (DCT-II of the log-mel frames)
 *   + 3 pitch stats (F0 mean/std in semitones re 100 Hz, voiced ratio at 8 kHz).
 * The 40 MFCC stats are z-scored with a hard-coded device-fitted table and
 * LDA-weighted, the pitch stats likewise; the two blocks are L2-normalised and
 * mixed 90/10.  The embedding is computed from the pre-AFE microphone signal
 * (see CONFIG_VOICE_VP_INPUT_RAW), not from the echo-cancelled ASR stream.
 *
 * Clustering is a small online state machine:
 *   - accept into the closest role at cosine >= VP_T_ACCEPT
 *   - otherwise buffer the vector as a *pending* candidate; a role 人N is only
 *     created after VP_PENDING_HITS hits inside VP_PENDING_TTL utterances
 *   - explicitly enrolled roles are frozen: never updated, never auto-merged,
 *     so an impostor can no longer be absorbed into "主人"
 *   - anonymous roles merge only with each other (>= VP_T_MERGE) and split when
 *     their exemplars stop being one blob (cohesion < VP_T_SPLIT)
 *
 * NOTE: the embedding is not compatible with the old 160-float .f32 files.
 * They are ignored on load; 主人 has to be enrolled again (one sentence).
 */

#define VP_MEL           80
#define VP_MFCC          20
#define VP_STATS         (VP_MFCC * 2)
#define VP_PITCH_DIM     3
#define VP_DIM           (VP_STATS + VP_PITCH_DIM)     /* 43 */

#define VP_MIN_SAMPLES   8000      /* 0.5 s */
#define VP_MAX_SAMPLES   96000     /* 6 s */
#define VP_MIN_FRAMES    20

#define VP_PITCH_WEIGHT  0.10f     /* energy share of the pitch block */
#define VP_EXEMPLARS     4

#define VP_PENDING_MAX   6
#define VP_PENDING_HITS  2
#define VP_PENDING_TTL   12
#define VP_T_PENDING     0.50f
#define VP_T_MERGE       0.70f
#define VP_T_SPLIT       0.45f
#define VP_T_ACCEPT      ((float)CONFIG_VOICE_SPEAKER_THRESHOLD / 100.0f)

/* Pitch front end: 16 kHz -> 8 kHz (pair average), 30 ms / 20 ms frames,
 * direct normalised autocorrelation over lags 26..115 (69.6..307.7 Hz). */
#define PITCH_SR         8000
#define PITCH_WIN        240
#define PITCH_STEP       160
#define PITCH_LAG_LO     26
#define PITCH_LAG_HI     115

typedef struct {
    char  name[SPEAKER_NAME_LEN];
    float *vec;                 /* VP_DIM floats, PSRAM */
    float *ex;                  /* VP_EXEMPLARS * VP_DIM floats, PSRAM */
    int    ex_count;
    int    ex_head;
    uint16_t count;
    bool   anon;
} role_t;

typedef struct {
    float vec[VP_DIM];
    int   count;
    int   last_idx;
    bool  used;
} pending_t;

static const char *TAG = "voiceprint";

static dl::audio::Fbank *s_fbank;
static bool s_dump_raw;          /* print VPVEC lines for table calibration */
static role_t s_db[SPEAKER_MAX];

/* Multi-utterance enrollment: average the feature vectors of N utterances
 * before freezing the role, which cuts within-speaker variance noticeably.
 * The console drives begin/add.../commit via main.c ("enroll <name>"). */
#define VP_ENROLL_MAX 8
static char  s_enroll_name[SPEAKER_NAME_LEN];
static float s_enroll_sum[VP_DIM];
static int   s_enroll_n;
/* Runtime override of the relative-margin gate (-1 = use Kconfig default). */
static int   s_margin_override_x100 = -1;
static int s_count;
static pending_t s_pending[VP_PENDING_MAX];
static int s_utt_idx;
static char s_last[SPEAKER_NAME_LEN];

/* ------------------------------ basics ---------------------------------- */

static void path_of(const char *name, char *out, size_t out_len)
{
    snprintf(out, out_len, BSP_SD_MOUNT_POINT "/voices/%.*s.f32",
             SPEAKER_NAME_LEN - 1, name);
}

static bool is_anon(const char *n)
{
    return strncmp(n, "人", 3) == 0;
}

static void l2norm(float *v, int n)
{
    float s = 0.0f;
    for (int i = 0; i < n; i++) {
        s += v[i] * v[i];
    }
    s = sqrtf(s);
    if (s > 1e-6f) {
        for (int i = 0; i < n; i++) {
            v[i] /= s;
        }
    }
}

static float cosine(const float *a, const float *b)
{
    float d = 0.0f;
    for (int i = 0; i < VP_DIM; i++) {
        d += a[i] * b[i];
    }
    return d;   /* unit vectors */
}

static float *psram_floats(int n)
{
    float *p = (float *)heap_caps_malloc((size_t)n * sizeof(float),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = (float *)malloc((size_t)n * sizeof(float));
    }
    return p;
}

/* --------------------------- feature extraction -------------------------- */

/* Pitch statistics: [F0 mean (st), F0 std (st), voiced ratio]. */
static void pitch_stats(const int16_t *pcm, size_t samples, float out[VP_PITCH_DIM])
{
    out[0] = out[1] = out[2] = 0.0f;
    size_t n8 = samples / 2;
    if (n8 < (size_t)(PITCH_WIN + 2 * PITCH_STEP)) {
        return;
    }
    int frames = 1 + (int)((n8 - PITCH_WIN) / PITCH_STEP);
    int voiced = 0;
    double sum = 0.0, sumsq = 0.0;
    const int nlags = PITCH_LAG_HI - PITCH_LAG_LO + 1;

    for (int t = 0; t < frames; t++) {
        float fr[PITCH_WIN];
        float mean = 0.0f;
        for (int i = 0; i < PITCH_WIN; i++) {
            size_t idx = 2 * ((size_t)t * PITCH_STEP + i);
            fr[i] = ((float)pcm[idx] + (float)pcm[idx + 1]) * (0.5f / 32768.0f);
            mean += fr[i];
        }
        mean /= PITCH_WIN;
        float r0 = 0.0f;
        for (int i = 0; i < PITCH_WIN; i++) {
            fr[i] -= mean;
            r0 += fr[i] * fr[i];
        }
        if (r0 < 1e-9f) {
            continue;
        }
        /* ac[k] for lag PITCH_LAG_LO-1+k, one extra on each side for the
         * parabolic interpolation below. */
        float ac[PITCH_LAG_HI - PITCH_LAG_LO + 3];
        int ac_n = (int)(sizeof(ac) / sizeof(ac[0]));
        for (int k = 0; k < ac_n; k++) {
            int lag = PITCH_LAG_LO - 1 + k;
            float s = 0.0f;
            int cnt = PITCH_WIN - lag;
            for (int i = 0; i < cnt; i++) {
                s += fr[i] * fr[i + lag];
            }
            ac[k] = s / r0;
        }
        int best = 1;
        for (int k = 2; k <= ac_n - 2; k++) {
            if (ac[k] > ac[best]) {
                best = k;
            }
        }
        if (ac[best] <= 0.45f) {
            continue;
        }
        float y0 = ac[best - 1], y1 = ac[best], y2 = ac[best + 1];
        float den = y0 - 2.0f * y1 + y2;
        float d = 0.0f;
        if (fabsf(den) > 1e-9f) {
            d = 0.5f * (y0 - y2) / den;
            if (fabsf(d) > 0.5f) {
                d = 0.0f;
            }
        }
        int lag_centered = PITCH_LAG_LO + (best - 1);
        float f0 = (float)PITCH_SR / ((float)lag_centered + d);
        if (f0 < 60.0f || f0 > 400.0f) {
            continue;
        }
        float st = 12.0f * log2f(f0 / 100.0f);
        sum += st;
        sumsq += (double)st * st;
        voiced++;
    }
    if (voiced < 3) {
        return;
    }
    float m = (float)(sum / voiced);
    float var = (float)(sumsq / voiced) - m * m;
    if (var < 0.0f) {
        var = 0.0f;
    }
    out[0] = m;
    out[1] = sqrtf(var);
    out[2] = (float)voiced / (float)frames;
}

/* Builds the 43-dim unit embedding.  Returns false on unusable audio. */
static bool build_feature(const int16_t *pcm, size_t samples, float *out)
{
    if (!s_fbank || !pcm || !out || samples < VP_MIN_SAMPLES) {
        return false;
    }
    if (samples > VP_MAX_SAMPLES) {
        samples = VP_MAX_SAMPLES;
    }
    std::vector<int> shape = s_fbank->get_output_shape((int)samples);
    if (shape.size() != 2) {
        return false;
    }
    int frames = shape[0];
    int dim = shape[1];
    if (frames < VP_MIN_FRAMES || dim != VP_MEL) {
        ESP_LOGW(TAG, "bad feature shape %dx%d", frames, dim);
        return false;
    }
    float *feat = (float *)heap_caps_malloc((size_t)frames * dim * sizeof(float),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!feat) {
        ESP_LOGE(TAG, "no memory for %d frames", frames);
        return false;
    }
    if (s_fbank->process(pcm, (int)samples, feat) != ESP_OK) {
        free(feat);
        return false;
    }

    double sum[VP_MFCC];
    double sumsq[VP_MFCC];
    for (int c = 0; c < VP_MFCC; c++) {
        sum[c] = 0.0;
        sumsq[c] = 0.0;
    }
    for (int t = 0; t < frames; t++) {
        const float *fr = feat + (size_t)t * dim;
        for (int c = 0; c < VP_MFCC; c++) {
            const float *row = VP_DCT[c];
            float acc = 0.0f;
            for (int j = 0; j < VP_MEL; j++) {
                acc += fr[j] * row[j];
            }
            sum[c] += acc;
            sumsq[c] += (double)acc * acc;
        }
    }
    free(feat);

    float zb[VP_STATS];
    float nb = 0.0f;
    for (int c = 0; c < VP_MFCC; c++) {
        float m = (float)(sum[c] / frames);
        float var = (float)(sumsq[c] / frames) - m * m;
        if (var < 0.0f) {
            var = 0.0f;
        }
        float raw_m = (m - VP_MFCC_MU[c]) / VP_MFCC_SD[c];
        float raw_s = (sqrtf(var) - VP_MFCC_MU[VP_MFCC + c]) / VP_MFCC_SD[VP_MFCC + c];
        /* LDA-style weighting: suppress dimensions that vary a lot within a
         * speaker, boost the discriminative ones (fitted on board recordings). */
        raw_m *= VP_LDA_W[c];
        raw_s *= VP_LDA_W[VP_MFCC + c];
        zb[c] = raw_m;
        zb[VP_MFCC + c] = raw_s;
        nb += raw_m * raw_m + raw_s * raw_s;
    }
    nb = sqrtf(nb);
    if (nb < 1e-6f) {
        return false;
    }

    float ps[VP_PITCH_DIM];
    pitch_stats(pcm, samples, ps);

    /* Diagnostic hook: emit the 40 MFCC mean/std values and the 3 pitch stats
     * before standardisation, for feature-table calibration.  Serial command
     * "vpvec on". */
    if (s_dump_raw) {
        char line[768];
        size_t off = (size_t)snprintf(line, sizeof(line), "VPVEC");
        for (int c = 0; c < VP_MFCC && off + 12 < sizeof(line); c++) {
            off += (size_t)snprintf(line + off, sizeof(line) - off, " %.4f",
                                    (double)(float)(sum[c] / frames));
        }
        for (int c = 0; c < VP_MFCC && off + 12 < sizeof(line); c++) {
            double var = (sumsq[c] / frames) - ((sum[c] / frames) * (sum[c] / frames));
            if (var < 0.0) {
                var = 0.0;
            }
            off += (size_t)snprintf(line + off, sizeof(line) - off, " %.4f",
                                    (double)sqrtf((float)var));
        }
        for (int k = 0; k < VP_PITCH_DIM && off + 12 < sizeof(line); k++) {
            off += (size_t)snprintf(line + off, sizeof(line) - off, " %.4f", (double)ps[k]);
        }
        printf("%s\n", line);
    }
    float zp[VP_PITCH_DIM];
    float np = 0.0f;
    for (int k = 0; k < VP_PITCH_DIM; k++) {
        zp[k] = (ps[k] - VP_PITCH_MU[k]) / VP_PITCH_SD[k];
        /* LDA-style pitch weighting (fitted with the MFCC weights). */
        zp[k] *= VP_LDA_PITCH_W[k];
        np += zp[k] * zp[k];
    }
    np = sqrtf(np);

    float wb = sqrtf(1.0f - VP_PITCH_WEIGHT) / nb;
    float wp = (np > 1e-6f) ? sqrtf(VP_PITCH_WEIGHT) / np : 0.0f;
    for (int d = 0; d < VP_STATS; d++) {
        out[d] = zb[d] * wb;
    }
    for (int k = 0; k < VP_PITCH_DIM; k++) {
        out[VP_STATS + k] = zp[k] * wp;
    }
    l2norm(out, VP_DIM);
    return true;
}

/* ------------------------------- storage -------------------------------- */

static void save_entry(int i)
{
    if (i < 0 || i >= s_count) {
        return;
    }
    mkdir(BSP_SD_MOUNT_POINT "/voices", 0777);
    char path[140];
    path_of(s_db[i].name, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot write %s", path);
        return;
    }
    fwrite(s_db[i].vec, sizeof(float), VP_DIM, f);
    fclose(f);
}

static void free_role(role_t *r)
{
    free(r->vec);
    free(r->ex);
    r->vec = NULL;
    r->ex = NULL;
}

static void db_load(void)
{
    mkdir(BSP_SD_MOUNT_POINT "/voices", 0777);
    DIR *d = opendir(BSP_SD_MOUNT_POINT "/voices");
    if (!d) {
        ESP_LOGW(TAG, "no voices directory yet");
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_count < SPEAKER_MAX) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".f32") != 0) {
            continue;
        }
        char name[SPEAKER_NAME_LEN];
        size_t n = (size_t)(dot - e->d_name);
        if (n == 0 || n >= sizeof(name)) {
            continue;
        }
        memcpy(name, e->d_name, n);
        name[n] = 0;

        char path[140];
        path_of(name, path, sizeof(path));
        FILE *f = fopen(path, "rb");
        if (!f) {
            continue;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz != (long)(VP_DIM * sizeof(float))) {
            ESP_LOGW(TAG, "removing stale voiceprint '%s': %ld bytes != %u "
                     "(old feature space) - re-enroll this person", name, sz,
                     (unsigned)(VP_DIM * sizeof(float)));
            fclose(f);
            remove(path);
            continue;
        }
        float *vec = psram_floats(VP_DIM);
        float *ex = psram_floats(VP_EXEMPLARS * VP_DIM);
        if (!vec || !ex) {
            free(vec);
            free(ex);
            fclose(f);
            continue;
        }
        if (fread(vec, sizeof(float), VP_DIM, f) != VP_DIM) {
            free(vec);
            free(ex);
            fclose(f);
            continue;
        }
        fclose(f);
        l2norm(vec, VP_DIM);
        memcpy(ex, vec, sizeof(float) * VP_DIM);
        snprintf(s_db[s_count].name, SPEAKER_NAME_LEN, "%.*s",
                 SPEAKER_NAME_LEN - 1, name);
        s_db[s_count].vec = vec;
        s_db[s_count].ex = ex;
        s_db[s_count].ex_count = 1;
        s_db[s_count].ex_head = 1 % VP_EXEMPLARS;
        s_db[s_count].count = 1;
        s_db[s_count].anon = is_anon(s_db[s_count].name);
        s_count++;
        ESP_LOGI(TAG, "loaded voiceprint '%s'%s", name,
                 s_db[s_count - 1].anon ? " (auto role)" : "");
    }
    closedir(d);
    ESP_LOGI(TAG, "%d voiceprint(s) loaded", s_count);
}

/* ------------------------------ clustering ------------------------------ */

static void push_exemplar(role_t *r, const float *v)
{
    if (!r->ex) {
        return;
    }
    memcpy(r->ex + (size_t)r->ex_head * VP_DIM, v, sizeof(float) * VP_DIM);
    r->ex_head = (r->ex_head + 1) % VP_EXEMPLARS;
    if (r->ex_count < VP_EXEMPLARS) {
        r->ex_count++;
    }
}

static bool next_anon_name(char *out)
{
    for (int n = 1; n < 200; n++) {
        char nm[SPEAKER_NAME_LEN];
        snprintf(nm, sizeof(nm), "人%d", n);
        bool used = false;
        for (int i = 0; i < s_count; i++) {
            if (strcmp(s_db[i].name, nm) == 0) {
                used = true;
                break;
            }
        }
        if (!used) {
            snprintf(out, SPEAKER_NAME_LEN, "%s", nm);
            return true;
        }
    }
    return false;
}

/* Creates a role from one unit vector.  Returns its index, or -1. */
static int create_role(const float *vec, uint16_t count, bool anon)
{
    if (s_count >= SPEAKER_MAX) {
        ESP_LOGW(TAG, "speaker table full, dropping role");
        return -1;
    }
    char name[SPEAKER_NAME_LEN];
    if (anon) {
        if (!next_anon_name(name)) {
            return -1;
        }
    } else {
        return -1;
    }
    float *v = psram_floats(VP_DIM);
    float *ex = psram_floats(VP_EXEMPLARS * VP_DIM);
    if (!v || !ex) {
        free(v);
        free(ex);
        return -1;
    }
    memcpy(v, vec, sizeof(float) * VP_DIM);
    l2norm(v, VP_DIM);
    memcpy(ex, v, sizeof(float) * VP_DIM);
    int idx = s_count++;
    snprintf(s_db[idx].name, SPEAKER_NAME_LEN, "%s", name);
    s_db[idx].vec = v;
    s_db[idx].ex = ex;
    s_db[idx].ex_count = 1;
    s_db[idx].ex_head = 1 % VP_EXEMPLARS;
    s_db[idx].count = count < 1 ? 1 : count;
    s_db[idx].anon = anon;
    save_entry(idx);
    ESP_LOGW(TAG, "new speaker role '%s' created (count %u)", name,
             (unsigned)s_db[idx].count);
    return idx;
}

static void update_centroid(role_t *r, const float *v)
{
    float a = 1.0f / ((float)r->count + 3.0f);
    if (a < 0.05f) {
        a = 0.05f;
    }
    for (int d = 0; d < VP_DIM; d++) {
        r->vec[d] = (1.0f - a) * r->vec[d] + a * v[d];
    }
    l2norm(r->vec, VP_DIM);
}

/* Anonymous roles only, never touch explicitly enrolled people. */
static void try_merge(void)
{
    bool merged = true;
    while (merged) {
    merged = false;
    for (int i = 0; i < s_count && !merged; i++) {
        for (int j = i + 1; j < s_count; j++) {
            role_t *a = &s_db[i];
            role_t *b = &s_db[j];
            if (!a->anon || !b->anon) {
                continue;
            }
            if (a->count < 2 || b->count < 2) {
                continue;
            }
            float sim = cosine(a->vec, b->vec);
            if (sim < VP_T_MERGE) {
                continue;
            }
            int keep = (a->count >= b->count) ? i : j;
            int gone = (keep == i) ? j : i;
            role_t *k = &s_db[keep];
            role_t *g = &s_db[gone];
            uint32_t total = (uint32_t)k->count + g->count;
            for (int d = 0; d < VP_DIM; d++) {
                k->vec[d] = ((float)k->count * k->vec[d] + (float)g->count * g->vec[d]) /
                            (float)total;
            }
            l2norm(k->vec, VP_DIM);
            k->count = (uint16_t)(total > 65535 ? 65535 : total);
            for (int e = 0; e < g->ex_count && k->ex_count < VP_EXEMPLARS; e++) {
                push_exemplar(k, g->ex + (size_t)e * VP_DIM);
            }
            ESP_LOGW(TAG, "merged role '%s' into '%s' (sim %.3f)",
                     g->name, k->name, sim);
            char path[140];
            path_of(g->name, path, sizeof(path));
            remove(path);
            free_role(g);
            for (int m = gone; m + 1 < s_count; m++) {
                s_db[m] = s_db[m + 1];
            }
            s_count--;
            save_entry(keep < gone ? keep : keep - 1);
            merged = true;
            break;
        }
    }
    }
}

/* Splits an anonymous role whose exemplars no longer look like one person. */
static void try_split(int idx)
{
    if (idx < 0 || idx >= s_count) {
        return;
    }
    role_t *r = &s_db[idx];
    if (!r->anon || r->count < 4 || r->ex_count < 3) {
        return;
    }
    int n = r->ex_count;
    float cohesion = 0.0f;
    for (int e = 0; e < n; e++) {
        cohesion += cosine(r->vec, r->ex + (size_t)e * VP_DIM);
    }
    cohesion /= (float)n;
    if (cohesion >= VP_T_SPLIT) {
        return;
    }

    float c0[VP_DIM], c1[VP_DIM];
    int a = 0;
    float worst = 2.0f;
    for (int e = 0; e < n; e++) {
        float s = cosine(r->vec, r->ex + (size_t)e * VP_DIM);
        if (s < worst) {
            worst = s;
            a = e;
        }
    }
    int b = -1;
    float far = 2.0f;
    for (int e = 0; e < n; e++) {
        if (e == a) {
            continue;
        }
        float s = cosine(r->ex + (size_t)a * VP_DIM, r->ex + (size_t)e * VP_DIM);
        if (s < far) {
            far = s;
            b = e;
        }
    }
    if (b < 0) {
        return;
    }
    memcpy(c0, r->ex + (size_t)a * VP_DIM, sizeof(float) * VP_DIM);
    memcpy(c1, r->ex + (size_t)b * VP_DIM, sizeof(float) * VP_DIM);

    int assign[VP_EXEMPLARS];
    int n0 = 0, n1 = 0;
    for (int iter = 0; iter < 5; iter++) {
        float s0[VP_DIM] = { 0 }, s1[VP_DIM] = { 0 };
        n0 = n1 = 0;
        for (int e = 0; e < n; e++) {
            const float *v = r->ex + (size_t)e * VP_DIM;
            float d0 = cosine(v, c0);
            float d1 = cosine(v, c1);
            if (d0 >= d1) {
                assign[e] = 0;
                for (int d = 0; d < VP_DIM; d++) {
                    s0[d] += v[d];
                }
                n0++;
            } else {
                assign[e] = 1;
                for (int d = 0; d < VP_DIM; d++) {
                    s1[d] += v[d];
                }
                n1++;
            }
        }
        if (n0 == 0 || n1 == 0) {
            return;
        }
        for (int d = 0; d < VP_DIM; d++) {
            c0[d] = s0[d] / (float)n0;
            c1[d] = s1[d] / (float)n1;
        }
        l2norm(c0, VP_DIM);
        l2norm(c1, VP_DIM);
    }
    if (n0 < 2 || n1 < 2) {
        return;
    }
    if (cosine(c0, c1) >= VP_T_MERGE - 0.05f) {
        return;
    }

    /* The role table is fixed-size: without this check a split made after all
     * SPEAKER_MAX slots are used writes past s_db[] and corrupts the heap. */
    if (s_count >= SPEAKER_MAX) {
        return;
    }
    char nm[SPEAKER_NAME_LEN];
    if (!next_anon_name(nm)) {
        return;
    }
    float *nv = psram_floats(VP_DIM);
    float *nex = psram_floats(VP_EXEMPLARS * VP_DIM);
    if (!nv || !nex) {
        free(nv);
        free(nex);
        return;
    }
    memcpy(nv, c1, sizeof(float) * VP_DIM);
    int ni = s_count++;
    snprintf(s_db[ni].name, SPEAKER_NAME_LEN, "%s", nm);
    s_db[ni].vec = nv;
    s_db[ni].ex = nex;
    s_db[ni].ex_count = 0;
    s_db[ni].ex_head = 0;
    s_db[ni].count = (uint16_t)n1;
    s_db[ni].anon = true;

    /* Rebuild both exemplar rings from the 2-means assignment. */
    r->ex_count = 0;
    r->ex_head = 0;
    for (int e = 0; e < n; e++) {
        const float *v = r->ex + (size_t)e * VP_DIM;
        if (assign[e] == 0) {
            push_exemplar(r, v);
        } else {
            push_exemplar(&s_db[ni], v);
        }
    }
    memcpy(r->vec, c0, sizeof(float) * VP_DIM);
    r->count = (uint16_t)n0;
    save_entry(idx);
    save_entry(ni);
    ESP_LOGW(TAG, "split role '%s' into '%s' (%d+%d members, sim %.2f)",
             r->name, nm, n0, n1, cosine(c0, c1));
}

/* ------------------------------ public API ------------------------------ */

esp_err_t speaker_id_init(void)
{
    dl::audio::SpeechFeatureConfig cfg;
    cfg.sample_rate = 16000;
    cfg.num_mel_bins = VP_MEL;
    cfg.frame_length = 25.0f;
    cfg.frame_shift = 10.0f;
    cfg.window_type = dl::audio::WinType::HAMMING;
    cfg.use_energy = false;
    cfg.use_log_fbank = 1;
    cfg.low_freq = 20.0f;
    cfg.high_freq = 0.0f;

    s_fbank = new dl::audio::Fbank(cfg);
    if (!s_fbank) {
        ESP_LOGE(TAG, "Fbank allocation failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "voiceprint ready: %d-dim (MFCC mean+std + pitch), "
             "accept %.2f merge %.2f split %.2f",
             VP_DIM, VP_T_ACCEPT, VP_T_MERGE, VP_T_SPLIT);
    db_load();
    snprintf(s_last, sizeof(s_last), "%s", SPEAKER_UNKNOWN);
    return ESP_OK;
}

bool speaker_id_ready(void)
{
    return s_fbank != NULL;
}

/* Freeze one embedding into (or over) an explicitly enrolled role. */
static esp_err_t store_enrolled(const char *name, const float *vec_in)
{
    float *vec = psram_floats(VP_DIM);
    if (!vec) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(vec, vec_in, sizeof(float) * VP_DIM);

    int slot = -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_db[i].name, name) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (s_count >= SPEAKER_MAX) {
            free(vec);
            return ESP_ERR_NO_MEM;
        }
        slot = s_count++;
        float *ex = psram_floats(VP_EXEMPLARS * VP_DIM);
        if (!ex) {
            s_count--;
            free(vec);
            return ESP_ERR_NO_MEM;
        }
        snprintf(s_db[slot].name, SPEAKER_NAME_LEN, "%.*s",
                 SPEAKER_NAME_LEN - 1, name);
        s_db[slot].vec = vec;
        s_db[slot].ex = ex;
        s_db[slot].ex_count = 0;
        s_db[slot].ex_head = 0;
        s_db[slot].count = 1;
        s_db[slot].anon = false;
    } else {
        free(s_db[slot].vec);
        s_db[slot].vec = vec;
        s_db[slot].count = 1;
        s_db[slot].anon = false;
        s_db[slot].ex_count = 0;
        s_db[slot].ex_head = 0;
    }
    for (int k = 0; k < VP_PENDING_MAX; k++) {
        s_pending[k].used = false;
    }
    push_exemplar(&s_db[slot], vec);
    save_entry(slot);
    ESP_LOGI(TAG, "enrolled '%s' (dim %d)", name, VP_DIM);
    return ESP_OK;
}

/* --------------------- multi-utterance enrollment API -------------------- */

esp_err_t speaker_id_enroll_begin(const char *name)
{
    if (!s_fbank || !name || !name[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(s_enroll_name, sizeof(s_enroll_name), "%s", name);
    memset(s_enroll_sum, 0, sizeof(s_enroll_sum));
    s_enroll_n = 0;
    return ESP_OK;
}

esp_err_t speaker_id_enroll_add(const int16_t *pcm, size_t samples)
{
    if (!s_enroll_name[0] || s_enroll_n >= VP_ENROLL_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    float v[VP_DIM];
    if (!build_feature(pcm, samples, v)) {
        ESP_LOGW(TAG, "could not build a voiceprint from %u samples",
                 (unsigned)samples);
        return ESP_ERR_INVALID_SIZE;
    }
    for (int d = 0; d < VP_DIM; d++) {
        s_enroll_sum[d] += v[d];
    }
    s_enroll_n++;
    return ESP_OK;
}

int speaker_id_enroll_progress(void)
{
    return s_enroll_n;
}

esp_err_t speaker_id_enroll_commit(void)
{
    if (!s_enroll_name[0] || s_enroll_n == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    float avg[VP_DIM];
    for (int d = 0; d < VP_DIM; d++) {
        avg[d] = s_enroll_sum[d] / (float)s_enroll_n;
    }
    l2norm(avg, VP_DIM);
    esp_err_t err = store_enrolled(s_enroll_name, avg);
    ESP_LOGW(TAG, "enroll '%s' averaged over %d utterance(s)",
             s_enroll_name, s_enroll_n);
    s_enroll_name[0] = 0;
    s_enroll_n = 0;
    return err;
}

void speaker_id_enroll_cancel(void)
{
    s_enroll_name[0] = 0;
    s_enroll_n = 0;
}

esp_err_t speaker_id_enroll(const char *name, const int16_t *pcm, size_t samples)
{
    esp_err_t err = speaker_id_enroll_begin(name);
    if (err != ESP_OK) {
        return err;
    }
    err = speaker_id_enroll_add(pcm, samples);
    if (err != ESP_OK) {
        speaker_id_enroll_cancel();
        return err;
    }
    return speaker_id_enroll_commit();
}

const char *speaker_id_identify(const int16_t *pcm, size_t samples)
{
    snprintf(s_last, sizeof(s_last), "%s", SPEAKER_UNKNOWN);
    if (!s_fbank) {
        return s_last;
    }
    float v[VP_DIM];
    if (!build_feature(pcm, samples, v)) {
        return s_last;
    }
    s_utt_idx++;

    if (s_count == 0) {
        int i = create_role(v, 1, true);
        if (i >= 0) {
            snprintf(s_last, sizeof(s_last), "%s", s_db[i].name);
        }
        return s_last;
    }

    int bi = -1;
    float best = -1.0f;
    float second = -1.0f;
    for (int i = 0; i < s_count; i++) {
        float s = cosine(v, s_db[i].vec);
        if (s > best) {
            second = best;
            best = s;
            bi = i;
        } else if (s > second) {
            second = s;
        }
    }

    /* Optional relative-margin gate (CONFIG_VOICE_SPEAKER_MARGIN_X100): a match
     * must also beat the runner-up by this much.  On this board different
     * speakers can both exceed the absolute threshold, so the gap is the only
     * remaining signal once the features are right. 0 = disabled. */
    int margin_x100 = (s_margin_override_x100 >= 0) ? s_margin_override_x100
                                                     : CONFIG_VOICE_SPEAKER_MARGIN_X100;
    const float margin = (float)margin_x100 / 100.0f;
    bool accepted = (best >= VP_T_ACCEPT);
    if (accepted && margin > 0.0f && best - second < margin) {
        accepted = false;
    }

    if (accepted) {
        role_t *r = &s_db[bi];
        snprintf(s_last, sizeof(s_last), "%s", r->name);
        bool anon = r->anon;
        if (r->count < 65535) {
            r->count++;
        }
        if (anon) {
            update_centroid(r, v);
            push_exemplar(r, v);
            if ((r->count % 5) == 0) {
                save_entry(bi);
            }
            /* These two can move roles around, so look the role up by name
             * afterwards instead of using the stale index / pointer. */
            try_merge();
            int ri = -1;
            for (int i = 0; i < s_count; i++) {
                if (strcmp(s_db[i].name, s_last) == 0) {
                    ri = i;
                    break;
                }
            }
            try_split(ri);
        }
        ESP_LOGI(TAG, "matched '%s' (%.3f >= %.2f, gap %.3f)%s", s_last, best,
                 VP_T_ACCEPT, second < 0.0f ? 1.0f : best - second,
                 anon ? "" : " [enrolled]");
        return s_last;
    }

    /* Unknown so far: only create a role after two consistent hits. */
    int pi = -1;
    float pbest = -1.0f;
    for (int k = 0; k < VP_PENDING_MAX; k++) {
        if (!s_pending[k].used) {
            continue;
        }
        if (s_utt_idx - s_pending[k].last_idx > VP_PENDING_TTL) {
            s_pending[k].used = false;
            continue;
        }
        float s = cosine(v, s_pending[k].vec);
        if (s > pbest) {
            pbest = s;
            pi = k;
        }
    }
    if (pi >= 0 && pbest >= VP_T_PENDING) {
        pending_t *p = &s_pending[pi];
        for (int d = 0; d < VP_DIM; d++) {
            p->vec[d] = (p->vec[d] * (float)p->count + v[d]) / (float)(p->count + 1);
        }
        l2norm(p->vec, VP_DIM);
        p->count++;
        p->last_idx = s_utt_idx;
        if (p->count >= VP_PENDING_HITS) {
            int ni = create_role(p->vec, (uint16_t)p->count, true);
            p->used = false;
            if (ni >= 0) {
                snprintf(s_last, sizeof(s_last), "%s", s_db[ni].name);
                try_merge();
                int ri = -1;
                for (int i = 0; i < s_count; i++) {
                    if (strcmp(s_db[i].name, s_last) == 0) {
                        ri = i;
                        break;
                    }
                }
                try_split(ri);
            }
        } else {
            ESP_LOGI(TAG, "pending new voice (%d/%d)", p->count, VP_PENDING_HITS);
        }
        ESP_LOGI(TAG, "no match (best %.3f < %.2f)", best, VP_T_ACCEPT);
        return s_last;
    }

    /* Fresh pending candidate: evict the oldest one when full. */
    int slot = -1;
    for (int k = 0; k < VP_PENDING_MAX; k++) {
        if (!s_pending[k].used) {
            slot = k;
            break;
        }
    }
    if (slot < 0) {
        slot = 0;
        for (int k = 1; k < VP_PENDING_MAX; k++) {
            if (s_pending[k].last_idx < s_pending[slot].last_idx) {
                slot = k;
            }
        }
    }
    memcpy(s_pending[slot].vec, v, sizeof(float) * VP_DIM);
    s_pending[slot].count = 1;
    s_pending[slot].last_idx = s_utt_idx;
    s_pending[slot].used = true;
    ESP_LOGI(TAG, "no match (best %.3f < %.2f, gap %.3f) -> new pending voice",
             best, VP_T_ACCEPT, second < 0.0f ? 1.0f : best - second);
    return s_last;
}

const char *speaker_id_identify_or_cluster(const int16_t *pcm, size_t samples)
{
    return speaker_id_identify(pcm, samples);
}

void speaker_id_set_dump_raw(bool on)
{
    s_dump_raw = on;
    ESP_LOGW(TAG, "raw-feature dump %s", on ? "enabled" : "disabled");
}

bool speaker_id_dump_raw(void)
{
    return s_dump_raw;
}

void speaker_id_set_margin_x100(int x100)
{
    s_margin_override_x100 = (x100 < 0) ? -1 : x100;
    ESP_LOGW(TAG, "margin gate -> %d (x100)%s", speaker_id_margin_x100(),
             s_margin_override_x100 < 0 ? " [Kconfig default]" : " [runtime]");
}

int speaker_id_margin_x100(void)
{
    return (s_margin_override_x100 >= 0) ? s_margin_override_x100
                                         : CONFIG_VOICE_SPEAKER_MARGIN_X100;
}

void speaker_id_reset_clusters(void)
{
    for (int i = s_count - 1; i >= 0; i--) {
        if (!s_db[i].anon) {
            continue;
        }
        char path[140];
        path_of(s_db[i].name, path, sizeof(path));
        remove(path);
        free_role(&s_db[i]);
        for (int k = i; k + 1 < s_count; k++) {
            s_db[k] = s_db[k + 1];
        }
        s_count--;
    }
    for (int k = 0; k < VP_PENDING_MAX; k++) {
        s_pending[k].used = false;
    }
    s_utt_idx = 0;
    ESP_LOGW(TAG, "anonymous roles cleared");
}

int speaker_id_info(char *out, size_t len)
{
    if (!out || !len) {
        return 0;
    }
    int off = snprintf(out, len,
                       "接受阈值 %.2f / merge %.2f / split %.2f / margin %.2f，共 %d 个角色\n",
                       VP_T_ACCEPT, VP_T_MERGE, VP_T_SPLIT,
                       (float)speaker_id_margin_x100() / 100.0f, s_count);
    for (int i = 0; i < s_count && off < (int)len - 60; i++) {
        off += snprintf(out + off, len - (size_t)off, "  %s%s  命中 %u 次\n",
                        s_db[i].name,
                        s_db[i].anon ? " (自动创建)" : " (已登记)",
                        (unsigned)s_db[i].count);
    }
    return s_count;
}

int speaker_id_list(char names[][SPEAKER_NAME_LEN], int max)
{
    int n = s_count < max ? s_count : max;
    for (int i = 0; i < n; i++) {
        snprintf(names[i], SPEAKER_NAME_LEN, "%.*s",
                 SPEAKER_NAME_LEN - 1, s_db[i].name);
    }
    return n;
}

esp_err_t speaker_id_forget(const char *name)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_db[i].name, name) == 0) {
            char path[140];
            path_of(name, path, sizeof(path));
            remove(path);
            free_role(&s_db[i]);
            s_db[i] = s_db[s_count - 1];
            s_count--;
            ESP_LOGI(TAG, "forgot '%s'", name);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
