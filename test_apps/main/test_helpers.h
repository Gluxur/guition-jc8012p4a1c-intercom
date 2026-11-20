#pragma once

#include "driver/jpeg_decode.h"

#include <cstddef>
#include <cstdint>

// Mires embarquees (cf. EMBED_FILES du CMakeLists). Quatre quadrants
// rouge / vert / bleu / blanc :
//  - 320x240 : dimensions deja multiples de 16, chemin nominal ;
//  - 200x120 : largeur NON multiple de 16, exerce l'alignement du decodeur.
// `extern` est indispensable : en C++ un `const` a portee de namespace a une
// edition de liens INTERNE par defaut, chaque TU verrait alors son propre
// symbole non defini.
extern const uint8_t k320_start[] asm("_binary_quad_320x240_jpg_start");
extern const uint8_t k320_end[] asm("_binary_quad_320x240_jpg_end");
extern const uint8_t k200_start[] asm("_binary_quad_200x120_jpg_start");
extern const uint8_t k200_end[] asm("_binary_quad_200x120_jpg_end");

inline size_t len320() { return static_cast<size_t>(k320_end - k320_start); }
inline size_t len200() { return static_cast<size_t>(k200_end - k200_start); }

namespace tst {

// Image décodée par le moteur JPEG matériel.
struct Decoded {
  uint8_t *buf = nullptr;  // RGB565, alloué par jpeg_alloc_decoder_mem
  uint32_t width = 0;      // dimensions réelles de l'image
  uint32_t height = 0;
  uint32_t stride = 0;     // largeur alignée 16 px, EN PIXELS
  size_t size = 0;         // capacité du buffer allouée
  uint32_t decoded_bytes = 0; // out_size rendu par jpeg_decoder_process
};

/* Moteur JPEG partagé, créé une fois au démarrage du firmware de test.
 * Comme en production : le moteur exige de la RAM interne DMA, il ne faut pas
 * compter en trouver plus tard. nullptr si la création a échoué. */
jpeg_decoder_handle_t decoder();
bool init_decoder();

/* true si le BSP s'est initialisé. Les tests matériels s'arrêtent proprement
 * plutôt que de partir sur des handles nuls. */
bool bsp_ready();
void set_bsp_ready(bool ready);

/* Décode `jpeg` en RGB565 (ordre BGR = little-endian, celui du panneau).
 * false si l'entête ou le décodage échoue. Libérer avec free_decoded(). */
bool decode_asset(const uint8_t *jpeg, size_t len, Decoded *out);
void free_decoded(Decoded *d);

/* Pixel (x, y) en tenant compte du stride aligné 16 px du décodeur. */
uint16_t pixel_at(const Decoded &d, uint32_t x, uint32_t y);

// Composantes RGB565. Le décodeur sort du little-endian, donc une lecture
// uint16_t sur le P4 (RISC-V little-endian) donne directement RRRRRGGGGGGBBBBB.
constexpr int r5(uint16_t p) { return (p >> 11) & 0x1F; }
constexpr int g6(uint16_t p) { return (p >> 5) & 0x3F; }
constexpr int b5(uint16_t p) { return p & 0x1F; }

} // namespace tst
