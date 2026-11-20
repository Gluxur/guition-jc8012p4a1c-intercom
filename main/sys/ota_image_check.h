#pragma once

#include <cstddef>
#include <cstdint>

// Validation de l'en-tête d'une image applicative ESP32, sur un simple tampon.
//
// Module PUR : il n'inclut AUCUN header ESP-IDF — ni esp_app_format.h, ni
// esp_app_desc.h — sinon il ne compilerait pas sur PC et n'aurait pas sa place
// dans test/. Il travaille sur des décalages d'octets et ses propres
// constantes, vérifiées contre un binaire réel du projet (cf.
// test/vectors/app_header.bin).
//
// Disposition d'une image (bootloader_support/esp_app_format.h et
// esp_app_format/esp_app_desc.h) :
//   octet 0                       : magic de l'image, 0xE9
//   octets 0..23                  : esp_image_header_t (24 octets)
//   octets 24..31                 : esp_image_segment_header_t (8 octets)
//   octets 32..                   : esp_app_desc_t, dont :
//       +0  uint32 magic_word     = 0xABCD5432
//       +4  uint32 secure_version
//       +8  uint32 reserv1[2]
//       +16 char   version[32]
//       +48 char   project_name[32]
//       +80 char   time[16]
//       +96 char   date[16]
namespace ota_image {

inline constexpr uint8_t kImageMagic = 0xE9;
inline constexpr uint32_t kAppDescMagic = 0xABCD5432;
inline constexpr size_t kAppDescOffset = 24 + 8;
inline constexpr size_t kHeaderProbeBytes = kAppDescOffset + 112;

struct Info {
  char version[32];
  char project[32];
  char date[16];
};

// Renvoie true si `buf` est bien le début d'une image applicative ESP32 et
// remplit `out`. Les champs texte sont recopiés bornés et toujours terminés
// par '\0' — le tampon source, lui, ne l'est pas forcément.
[[nodiscard]] bool parse_header(const uint8_t *buf, size_t len, Info *out);

} // namespace ota_image
