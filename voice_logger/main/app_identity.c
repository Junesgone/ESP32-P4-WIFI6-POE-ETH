/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Junesgone
 */

#include "app_identity.h"

#include <stdio.h>

static const identity_provider_t *s_face;
static const identity_provider_t *s_speaker;
static char s_last_speaker[32] = IDENTITY_UNKNOWN;

void identity_set_last_speaker(const char *name)
{
    snprintf(s_last_speaker, sizeof(s_last_speaker), "%s", (name && name[0]) ? name : IDENTITY_UNKNOWN);
}

void identity_register_face(const identity_provider_t *provider)
{
    s_face = provider;
}

void identity_register_speaker(const identity_provider_t *provider)
{
    s_speaker = provider;
}

static const char *lookup(const identity_provider_t *p)
{
    if (!p || !p->identify) {
        return IDENTITY_UNKNOWN;
    }
    const char *id = p->identify(p->ctx);
    return id ? id : IDENTITY_UNKNOWN;
}

const char *identity_face_id(void)
{
    return lookup(s_face);
}

const char *identity_speaker_id(void)
{
    /* A registered provider (e.g. a cloud voiceprint service) wins if present;
     * otherwise use whatever the on-board esp-dl matcher decided. */
    if (s_speaker) {
        return lookup(s_speaker);
    }
    return s_last_speaker;
}

bool identity_face_available(void)
{
    return s_face != 0;
}
