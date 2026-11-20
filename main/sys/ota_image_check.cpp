#include "ota_image_check.h"

#include <cstring>

namespace ota_image {
namespace {

// Recopie bornée d'un champ texte du descripteur : la source n'est pas
// forcément terminée par '\0' (champ rempli au maximum).
void copy_field(char *dst, size_t dst_cap, const uint8_t *src, size_t src_len) {
  const size_t n = src_len < dst_cap - 1 ? src_len : dst_cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

} // namespace

bool parse_header(const uint8_t *buf, size_t len, Info *out) {
  if (buf == nullptr || out == nullptr || len < kHeaderProbeBytes) {
    return false;
  }
  if (buf[0] != kImageMagic) {
    return false;
  }
  const uint8_t *d = buf + kAppDescOffset;
  if (le32(d) != kAppDescMagic) {
    return false;
  }
  copy_field(out->version, sizeof(out->version), d + 16, 32);
  copy_field(out->project, sizeof(out->project), d + 48, 32);
  copy_field(out->date, sizeof(out->date), d + 96, 16);
  return true;
}

} // namespace ota_image
