#pragma once

#include <cstddef>
#include <cstdint>

// En-tête BMP pour une image RGB565 telle que LVGL la rend (pixels uint16
// petit-boutistes). BI_BITFIELDS 16 bits : les octets du tampon LVGL partent
// tels quels, sans boucle de conversion de couleur. Hauteur négative = lignes
// de haut en bas, l'ordre du tampon. Lu par Windows, navigateurs, GIMP.
// Logique pure, testée sur host (test/test_bmp_header.cpp).
namespace bmp {

inline constexpr size_t kRgb565HeaderBytes = 66; // 14 + 40 + 3 masques

// Octets d'une ligne dans le fichier : w * 2 arrondi au multiple de 4.
[[nodiscard]] size_t rgb565_row_bytes(int w);

// Remplit `out`. w, h > 0.
void rgb565_header(uint8_t out[kRgb565HeaderBytes], int w, int h);

} // namespace bmp
