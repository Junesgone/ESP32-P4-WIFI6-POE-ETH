#pragma once
/* ---------------------------------------------------------------------------
 * Identity providers - the seam reserved for face recognition and speaker
 * verification.
 *
 * Nothing here needs a camera today: with no provider registered both lookups
 * return IDENTITY_UNKNOWN and every log record stays valid.  When the MIPI-CSI
 * camera arrives, implement a provider with ESP-DL / ESP-WHO and register it:
 *
 *     static const char *my_face_lookup(void *ctx) { ... }
 *     static const identity_provider_t face = {
 *         .name = "esp-who", .identify = my_face_lookup, .ctx = NULL };
 *     identity_register_face(&face);
 *
 * The same shape works for speaker verification
 * (espressif/speaker_verification, sv_tdnn_tiny, ESP32-P4 supported).
 * ------------------------------------------------------------------------- */
#include <stdbool.h>

#define IDENTITY_UNKNOWN "unknown"

typedef struct {
    const char *name;
    /* Return a stable per-person id, or NULL when nobody is recognised.
     * The returned string must stay valid until the next call. */
    const char *(*identify)(void *ctx);
    void *ctx;
} identity_provider_t;

/* Result of the on-board esp-dl voiceprint matcher for the current utterance.
 * Call this right after identifying; handle_utterance() then records whatever
 * identity_speaker_id() returns. */
void identity_set_last_speaker(const char *name);

void identity_register_face(const identity_provider_t *provider);
void identity_register_speaker(const identity_provider_t *provider);

/* Never NULL: falls back to IDENTITY_UNKNOWN. */
const char *identity_face_id(void);
const char *identity_speaker_id(void);

/* True once a real camera-backed provider has been registered. */
bool identity_face_available(void);
