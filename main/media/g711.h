#pragma once

#include <stdint.h>

namespace g711 {

/* Remplit la table A-law -> PCM16. À appeler une fois au boot, avant tout
 * décodage audio. */
void init();

/* Encodage PCM16 -> G.711 A-law (RFC 3551). */
uint8_t pcm16_to_alaw(int16_t pcm_val);

/* Décodage G.711 A-law -> PCM16 par table (init() requis). */
int16_t alaw_to_pcm16(uint8_t alaw);

} // namespace g711
