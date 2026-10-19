#pragma once
/* Voiceprint (speaker identification + online clustering), fully on-board.
 *
 * The embedding is hand-built on top of esp-dl's Fbank: 20 MFCC mean + 20 MFCC
 * std + 3 pitch statistics -> 43 dims, standardised and L2-normalised (see
 * app_vp_tables.h for how it was derived).  It is computed from the pre-AFE
 * microphone signal (CONFIG_VOICE_VP_INPUT_RAW) and the MU/SD tables were
 * re-fitted on board recordings on 2026-10-19: same speaker 0.643 +- 0.220,
 * different speaker -0.142 +- 0.285 (d'=3.1, AUC 0.98).
 *
 * Enrolled people are frozen references (never updated, never auto-merged).
 * Unknown voices are buffered as pending candidates; two consistent hits
 * create an anonymous role 人N, which can later merge with other anonymous
 * roles or split.  Everything is persisted as 43-float .f32 files on the TF
 * card, so roles survive a reboot.
 *
 * The implementation lives in app_speaker_id.cpp because the esp-dl API is C++.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* The implementation is C++ (esp-dl API), the callers are C. */
#ifdef __cplusplus
extern "C" {
#endif

#define SPEAKER_UNKNOWN   "unknown"
#define SPEAKER_NAME_LEN  24
#define SPEAKER_MAX       16

esp_err_t speaker_id_init(void);
bool speaker_id_ready(void);

/* Save/overwrite a voiceprint from one utterance (16 kHz mono int16). */
esp_err_t speaker_id_enroll(const char *name, const int16_t *pcm, size_t samples);

/* Multi-utterance enrollment: begin(name) -> add() per utterance -> commit().
 * The commit averages the 43-dim vectors (then L2-normalises), which cuts
 * within-speaker variance; the console drives this with "enroll <name>". */
esp_err_t speaker_id_enroll_begin(const char *name);
esp_err_t speaker_id_enroll_add(const int16_t *pcm, size_t samples);
esp_err_t speaker_id_enroll_commit(void);
void      speaker_id_enroll_cancel(void);
int       speaker_id_enroll_progress(void);

/* Relative margin gate override (cosine x100, <0 = use Kconfig default). */
void speaker_id_set_margin_x100(int x100);
int  speaker_id_margin_x100(void);

/* Best match for this utterance, or SPEAKER_UNKNOWN. */
const char *speaker_id_identify(const int16_t *pcm, size_t samples);

/* Like speaker_id_identify(), but when nobody enrolled matches, fall back to
 * anonymous clusters so a multi-person conversation still gets stable labels:
 * "人1", "人2", ...  Returns the label (valid until the next call). */
const char *speaker_id_identify_or_cluster(const int16_t *pcm, size_t samples);

/* Forget the anonymous roles (start labelling from 人1 again). */
void speaker_id_reset_clusters(void);

/* Calibration aid: when enabled, each extracted feature vector is printed once
 * as "VPVEC <40 unstandardised MFCC stats> <3 pitch stats>" on the console.
 * Used to re-fit the standardisation tables from board-recorded audio (serial
 * command "vpvec on|off"). */
void speaker_id_set_dump_raw(bool on);
bool speaker_id_dump_raw(void);

/* Adaptive match threshold currently in use (learned from hits and misses). */
float speaker_id_threshold(void);

/* Human readable list of roles + the learned threshold; returns entry count. */
int speaker_id_info(char *out, size_t len);

/* Fills names[] and returns how many are enrolled. */
int speaker_id_list(char names[][SPEAKER_NAME_LEN], int max);
esp_err_t speaker_id_forget(const char *name);

#ifdef __cplusplus
}
#endif
