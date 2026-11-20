#include "ppa_srm_quirk.h"

namespace bsp::ppa_quirk {
namespace {

/* Constantes reprises telles quelles de ppa_srm.c (ESP-IDF 5.4.4) et de
 * ppa_ll.h, pour le seul cas produit par handle_flush : RGB565 en entrée
 * comme en sortie, échelle 1:1. */
constexpr std::uint32_t kFifoBits = 12 * 128; /* profondeur FIFO 2D-DMA */
constexpr std::uint32_t kOutDepthBits = 16;   /* RGB565 */
constexpr std::uint32_t kUnitW = 64;          /* 128 octets / 2 octets par px */
constexpr std::uint32_t kMbH = 16;            /* macro-bloc SRM 16 x 16 */
constexpr std::uint32_t kMinTailPx = kFifoBits / kOutDepthBits; /* 96 px */

/* Reste non nul : une dernière unité pleine compte pour l'unité entière, pas
 * pour zéro (`w_left = (w_left == 0) ? w_divisor : w_left` dans le driver). */
constexpr std::uint32_t rest(std::uint32_t v, std::uint32_t m) noexcept {
  const std::uint32_t r = v % m;
  return r == 0 ? m : r;
}

/* Le test du driver, appliqué à une géométrie de sortie donnée : l'image
 * est-elle découpée en plusieurs unités, et la dernière est-elle trop petite
 * pour vider la FIFO ? */
constexpr bool tail_starves_fifo(std::uint32_t out_w,
                                 std::uint32_t out_h) noexcept {
  return ((out_w > kUnitW) || (out_h > kMbH)) &&
         rest(out_w, kUnitW) * rest(out_h, kMbH) < kMinTailPx;
}

} // namespace

bool rot270_band_stalls(std::uint32_t w, std::uint32_t h) noexcept {
  if (w == 0 || h == 0) {
    return false;
  }
  /* Géométrie réellement émise après la rotation 270° : largeur h, hauteur w. */
  const bool needs_bypass = tail_starves_fifo(h, w);
  /* Ce que ppa_srm.c évalue : la bande d'entrée, rotation ignorée. */
  const bool driver_bypasses = tail_starves_fifo(w, h);
  return needs_bypass && !driver_bypasses;
}

std::uint32_t rot270_split(std::uint32_t w, std::uint32_t h,
                           std::uint32_t *heights, std::uint32_t cap) noexcept {
  if (heights == nullptr || cap == 0 || w == 0 || h == 0) {
    return 0;
  }
  if (!rot270_band_stalls(w, h)) {
    heights[0] = h;
    return 1;
  }

  /* Découpe en deux, en spirale autour du milieu : à sûreté égale on préfère
   * des moitiés équilibrées, chaque sous-bande coûtant une transaction. */
  if (cap >= 2) {
    const std::uint32_t mid = h / 2;
    for (std::uint32_t d = 0; d <= mid; ++d) {
      const std::uint32_t candidates[2] = {mid - d, mid + d};
      for (std::uint32_t h1 : candidates) {
        if (h1 == 0 || h1 >= h) {
          continue;
        }
        if (!rot270_band_stalls(w, h1) && !rot270_band_stalls(w, h - h1)) {
          heights[0] = h1;
          heights[1] = h - h1;
          return 2;
        }
      }
    }
  }

  /* Aucune coupe en deux ne convient : découpe gloutonne, la plus grande
   * sous-bande sûre d'abord, en refusant celle qui laisserait un reste
   * qu'aucune sous-bande sûre ne peut entamer. Borné par `cap` : le coût est
   * une transaction PPA par part. */
  /* Plus petite sous-bande sûre : un reste plus court qu'elle est inentamable,
   * ce qui remplace une recherche imbriquée (O(h²) dans le chemin de flush)
   * par une comparaison. */
  std::uint32_t min_safe = 0;
  for (std::uint32_t c = 1; c <= h; ++c) {
    if (!rot270_band_stalls(w, c)) {
      min_safe = c;
      break;
    }
  }
  if (min_safe == 0) {
    return 0;
  }

  std::uint32_t parts = 0;
  std::uint32_t rem = h;
  while (rem > 0 && parts < cap) {
    std::uint32_t chosen = 0;
    for (std::uint32_t c = rem; c >= 1; --c) {
      if (rot270_band_stalls(w, c)) {
        continue;
      }
      const std::uint32_t left = rem - c;
      if (left == 0 || left >= min_safe) {
        chosen = c;
        break;
      }
    }
    if (chosen == 0) {
      return 0;
    }
    heights[parts++] = chosen;
    rem -= chosen;
  }
  return rem == 0 ? parts : 0;
}

} // namespace bsp::ppa_quirk
