#include "premul_blend.h"

namespace premul_blend {

namespace {

constexpr uint16_t pack565(uint32_t r, uint32_t g, uint32_t b) {
  return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// x / 255 arrondi, exact pour x dans [0, 255*255].
constexpr uint32_t div255(uint32_t x) { return (x + 128 + ((x + 128) >> 8)) >> 8; }

// Prémultiplié : s <= a en théorie, mais un pixel hors norme ne doit pas
// déborder sur le canal voisin une fois packé.
constexpr uint32_t over(uint32_t s, uint32_t inv_a, uint32_t bg) {
  const uint32_t v = s + div255(inv_a * bg);
  return v > 255 ? 255 : v;
}

} // namespace

void over_rgb565(const uint32_t *src, uint16_t *dst, size_t n,
                 uint32_t bg_rgb) {
  const uint32_t br = (bg_rgb >> 16) & 0xFF;
  const uint32_t bgc = (bg_rgb >> 8) & 0xFF;
  const uint32_t bb = bg_rgb & 0xFF;
  const uint16_t bg565 = pack565(br, bgc, bb);
  for (size_t i = 0; i < n; i++) {
    const uint32_t p = src[i];
    const uint32_t a = p >> 24;
    // La plupart des pixels d'une icône sont du fond pur : chemin court.
    if (a == 0) {
      dst[i] = bg565;
      continue;
    }
    const uint32_t r = (p >> 16) & 0xFF;
    const uint32_t g = (p >> 8) & 0xFF;
    const uint32_t b = p & 0xFF;
    if (a == 255) {
      dst[i] = pack565(r, g, b);
      continue;
    }
    const uint32_t inv = 255 - a;
    dst[i] = pack565(over(r, inv, br), over(g, inv, bgc), over(b, inv, bb));
  }
}

} // namespace premul_blend
