#pragma once

#include <cstdint>

// Détection de mouvement par différence d'images, sur l'image BRUTE de
// l'OV02C10 (RAW10 compacté MIPI). Logique pure, sans IO ni driver : testée
// sur host (test/test_motion_detect.cpp), le matériel vit dans bsp_camera.cpp
// et la boucle dans app/presence.cpp.
namespace motion {

// Grille de luminance moyenne. 40 x 24 cellules sur 1280 x 720 = 32 x 30 px
// par cellule : assez fin pour qu'une personne à 4 m couvre plusieurs
// cellules, assez gros pour que la moyenne (~130 échantillons par cellule)
// écrase le bruit du capteur à fort gain.
inline constexpr int kGridW = 40;
inline constexpr int kGridH = 24;

struct Grid {
  uint8_t cell[kGridH][kGridW];
  uint8_t mean; // moyenne des cellules
};

// Réduit une image RAW10 compactée (stride = width*10/8 octets ; groupes de 5
// octets dont les 4 premiers sont les 8 bits de poids fort de 4 pixels) en
// grille. N'échantillonne qu'une ligne sur `row_step` et un groupe sur deux :
// ~25 % des octets lus, le reste ne changerait pas la moyenne d'une cellule.
// Le motif de Bayer (GBRG) est mélangé tel quel : seule compte la cohérence
// d'une image à l'autre. width doit être multiple de 8, height >= kGridH.
// NON réentrante (accumulateurs statiques) : un seul appelant.
void reduce_raw10(const uint8_t *frame, int width, int height, int row_step,
                  Grid &out);

struct Diff {
  int changed;  // cellules au-delà du seuil
  int max_delta; // plus grand écart, pour régler les seuils sur mesure
};

// Compare deux grilles EN RETIRANT l'écart de moyenne globale : un nuage qui
// passe ou un pas d'exposition décale toute l'image d'un bloc, une personne
// ne change qu'une région. Seuil en niveaux 8 bits par cellule.
[[nodiscard]] Diff compare(const Grid &prev, const Grid &cur, int threshold);

struct Exposure {
  uint32_t lines;
  uint32_t gain_x16; // 16 = x1
};

// Exposition automatique minimale : vise `target` (8 bits, piédestal du
// niveau de noir compris) en jouant d'abord sur le temps de pose puis sur le
// gain, par pas bornés (x0,5 .. x2) pour ne pas osciller. Rend `cur`
// inchangée dans la bande morte (+-15 %) : la comparaison suivante n'est
// valable que si l'exposition n'a pas bougé, chaque pas inutile coûte une
// image de détection.
[[nodiscard]] Exposure next_exposure(const Exposure &cur, uint8_t mean,
                                     uint8_t target, uint8_t black_level,
                                     uint32_t max_lines, uint32_t min_lines,
                                     uint32_t max_gain_x16);

} // namespace motion
