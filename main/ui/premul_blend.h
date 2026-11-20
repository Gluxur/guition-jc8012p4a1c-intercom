#pragma once

#include <cstddef>
#include <cstdint>

// Composition d'une image ARGB8888 PRÉMULTIPLIÉE (sortie ThorVG,
// a<<24 | r<<16 | g<<8 | b) sur un fond uni, vers du RGB565 natif
// (r5<<11 | g6<<5 | b5, l'ordre que LVGL lit pour LV_COLOR_FORMAT_RGB565).
// Pure, sans LVGL : testée sur host (test/test_premul_blend.cpp).
namespace premul_blend {

// bg_rgb = 0xRRGGBB.
void over_rgb565(const uint32_t *src, uint16_t *dst, size_t n,
                 uint32_t bg_rgb);

} // namespace premul_blend
