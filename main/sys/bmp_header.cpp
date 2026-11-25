#include "sys/bmp_header.h"

namespace bmp {
namespace {

void put16(uint8_t *p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void put32(uint8_t *p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

} // namespace

size_t rgb565_row_bytes(int w) {
  return (static_cast<size_t>(w) * 2 + 3) & ~static_cast<size_t>(3);
}

void rgb565_header(uint8_t out[kRgb565HeaderBytes], int w, int h) {
  const uint32_t image = static_cast<uint32_t>(rgb565_row_bytes(w) * h);
  // BITMAPFILEHEADER
  out[0] = 'B';
  out[1] = 'M';
  put32(out + 2, static_cast<uint32_t>(kRgb565HeaderBytes) + image);
  put32(out + 6, 0);
  put32(out + 10, static_cast<uint32_t>(kRgb565HeaderBytes));
  // BITMAPINFOHEADER
  put32(out + 14, 40);
  put32(out + 18, static_cast<uint32_t>(w));
  put32(out + 22, static_cast<uint32_t>(-h)); // négatif : de haut en bas
  put16(out + 26, 1);                         // plans
  put16(out + 28, 16);                        // bits par pixel
  put32(out + 30, 3);                         // BI_BITFIELDS
  put32(out + 34, image);
  put32(out + 38, 2835); // 72 dpi, en pixels par mètre
  put32(out + 42, 2835);
  put32(out + 46, 0);
  put32(out + 50, 0);
  // Masques R, G, B du RGB565
  put32(out + 54, 0xF800);
  put32(out + 58, 0x07E0);
  put32(out + 62, 0x001F);
}

} // namespace bmp
