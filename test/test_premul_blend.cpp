#include "ui/premul_blend.h"

#include <gtest/gtest.h>

// Valeurs attendues calculées à la main : r5 = r >> 3, g6 = g >> 2, b5 = b >> 3,
// sortie = r5 << 11 | g6 << 5 | b5. Composition prémultipliée :
// c = s + (255 - a) * fond / 255.

namespace {

uint16_t one(uint32_t px, uint32_t bg) {
  uint16_t out = 0xDEAD;
  premul_blend::over_rgb565(&px, &out, 1, bg);
  return out;
}

} // namespace

TEST(PremulBlend, TransparentGivesBackground) {
  // 0x2A1245 : 42>>3 = 5, 18>>2 = 4, 69>>3 = 8 -> 5<<11 | 4<<5 | 8.
  EXPECT_EQ(one(0x00000000u, 0x2A1245), 0x2888);
}

TEST(PremulBlend, OpaqueIgnoresBackground) {
  EXPECT_EQ(one(0xFFFF0000u, 0x00FF00), 0xF800);
  // 0x12 = 18 -> 2, 0x34 = 52 -> 13, 0x56 = 86 -> 10 : 4096 + 416 + 10.
  EXPECT_EQ(one(0xFF123456u, 0xFFFFFF), 0x11AA);
}

TEST(PremulBlend, HalfAlphaMixesWithBackground) {
  // Blanc prémultiplié à a = 128 : 128 sur noir -> 128 par canal.
  EXPECT_EQ(one(0x80808080u, 0x000000), 0x8410);
  // ... sur blanc : 128 + 127 * 255 / 255 = 255.
  EXPECT_EQ(one(0x80808080u, 0xFFFFFF), 0xFFFF);
}

TEST(PremulBlend, ChannelsComposeIndependently) {
  // Rouge prémultiplié a = 64 sur bleu pur : r = 64 -> 8, g = 0,
  // b = 191 * 255 / 255 = 191 -> 23.
  EXPECT_EQ(one(0x40400000u, 0x0000FF), 0x4017);
}

TEST(PremulBlend, DivisionIsBy255NotBy256) {
  // a = 7 sur blanc : 248 * 255 / 255 = 248 exactement (31/62/31). Un /256
  // donnerait 247 -> 30/61/30 = 0xF7BE : le fond virerait au gris.
  EXPECT_EQ(one(0x07000000u, 0xFFFFFF), 0xFFDF);
}

TEST(PremulBlend, OutOfRangePremulIsClampedNotWrapped) {
  // s > a (hors norme) : 255 + 239 doit plafonner à 255 et non déborder
  // sur les bits du canal voisin (0xE800 sans saturation).
  EXPECT_EQ(one(0x10FF0000u, 0xFF0000), 0xF800);
}

TEST(PremulBlend, ProcessesWholeSpan) {
  const uint32_t src[3] = {0x00000000u, 0xFFFF0000u, 0x80808080u};
  uint16_t dst[4] = {1, 2, 3, 0xBEEF};
  premul_blend::over_rgb565(src, dst, 3, 0x000000);
  EXPECT_EQ(dst[0], 0x0000);
  EXPECT_EQ(dst[1], 0xF800);
  EXPECT_EQ(dst[2], 0x8410);
  EXPECT_EQ(dst[3], 0xBEEF); // n respecté
}
