#include "g711.h"

#include <stddef.h>

namespace g711 {

// Décodage G711 A-law (PCMA) -> PCM16 par table précalculée (256 entrées,
// RFC 3551). go2rtc transcode la caméra en audio=pcma, pas pcmu.
static int16_t s_alaw_to_pcm16[256];

void init() {
  for (int i = 0; i < 256; i++) {
    uint8_t a = (uint8_t)(i ^ 0x55);
    int sign = (a & 0x80) ? 1 : -1;
    int exponent = (a >> 4) & 0x07;
    int mantissa = a & 0x0F;
    int magnitude;
    if (exponent == 0) {
      magnitude = (mantissa << 4) + 8;
    } else {
      magnitude = ((mantissa << 4) + 0x108) << (exponent - 1);
    }
    s_alaw_to_pcm16[i] = (int16_t)(sign * magnitude);
  }
}

int16_t alaw_to_pcm16(uint8_t alaw) { return s_alaw_to_pcm16[alaw]; }

// Encodage PCM16 -> G711 A-law (RFC 3551), implémentation de référence
// domaine public (Sun/ITU, reprise par Asterisk/SpanDSP). Symétrique à la
// table de décodage s_alaw_to_pcm16 ci-dessus : vérifié par round-trip au
// boot (self_test).
static int alaw_search_seg(int val, const int16_t *table, int size) {
  for (int i = 0; i < size; i++) {
    if (val <= table[i]) {
      return i;
    }
  }
  return size;
}

uint8_t pcm16_to_alaw(int16_t pcm_val) {
  static const int16_t kSegEnd[8] = {0x1F,  0x3F,  0x7F,  0xFF,
                                     0x1FF, 0x3FF, 0x7FF, 0xFFF};
  int mask;
  int v = pcm_val >> 3;
  if (v >= 0) {
    mask = 0xD5;
  } else {
    mask = 0x55;
    v = -v - 1;
  }
  int seg = alaw_search_seg(v, kSegEnd, 8);
  uint8_t aval;
  if (seg >= 8) {
    aval = 0x7F;
  } else {
    aval = (uint8_t)(seg << 4);
    if (seg < 2) {
      aval |= (v >> 1) & 0x0F;
    } else {
      aval |= (v >> seg) & 0x0F;
    }
  }
  return (uint8_t)(aval ^ mask);
}

} // namespace g711
