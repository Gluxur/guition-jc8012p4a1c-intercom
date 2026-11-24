#include <gtest/gtest.h>

#include "sys/bmp_header.h"

#include <cstdint>
#include <cstring>

// Expected bytes written by hand from the BMP format (BITMAPFILEHEADER +
// BITMAPINFOHEADER + BI_BITFIELDS masks), not produced by the code under test.
TEST(BmpHeader, Rgb565ThreeByTwo) {
  // 3 px * 2 B = 6 B per row, padded to 8; 2 rows = 16 B; file = 66 + 16 = 82.
  const uint8_t expected[bmp::kRgb565HeaderBytes] = {
      'B',  'M',                    // signature
      82,   0,    0,    0,          // file size
      0,    0,    0,    0,          // reserved
      66,   0,    0,    0,          // pixel data offset
      40,   0,    0,    0,          // info header size
      3,    0,    0,    0,          // width
      0xFE, 0xFF, 0xFF, 0xFF,       // height -2 (top-down)
      1,    0,                      // planes
      16,   0,                      // bpp
      3,    0,    0,    0,          // BI_BITFIELDS
      16,   0,    0,    0,          // image size
      0x13, 0x0B, 0,    0,          // 2835 px/m
      0x13, 0x0B, 0,    0,          // 2835 px/m
      0,    0,    0,    0,          // colours used
      0,    0,    0,    0,          // important colours
      0x00, 0xF8, 0,    0,          // red mask
      0xE0, 0x07, 0,    0,          // green mask
      0x1F, 0x00, 0,    0,          // blue mask
  };
  uint8_t got[bmp::kRgb565HeaderBytes];
  memset(got, 0xAA, sizeof got);
  bmp::rgb565_header(got, 3, 2);
  for (size_t i = 0; i < sizeof got; i++) {
    EXPECT_EQ(got[i], expected[i]) << "byte " << i;
  }
}

TEST(BmpHeader, ScreenSizeFileSize) {
  // 1280 x 800: 2560 B per row, no padding; 66 + 2 048 000 = 2 048 066 B.
  uint8_t h[bmp::kRgb565HeaderBytes];
  bmp::rgb565_header(h, 1280, 800);
  const uint32_t size = h[2] | h[3] << 8 | h[4] << 16 | (uint32_t)h[5] << 24;
  EXPECT_EQ(size, 2048066u);
  EXPECT_EQ(bmp::rgb565_row_bytes(1280), 2560u);
}

TEST(BmpHeader, RowPadding) {
  EXPECT_EQ(bmp::rgb565_row_bytes(1), 4u);
  EXPECT_EQ(bmp::rgb565_row_bytes(2), 4u);
  EXPECT_EQ(bmp::rgb565_row_bytes(3), 8u);
}
