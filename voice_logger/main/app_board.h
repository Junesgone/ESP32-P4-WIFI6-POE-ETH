#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Brings up the codec power amplifier enable line and any fixed board GPIOs. */
esp_err_t board_init(void);

/* Mute / unmute the NS4150B power amplifier (active high). */
void board_pa_enable(bool on);

/* Short two-tone chirp so the speaker path can be verified without a network. */
void board_beep(void);
