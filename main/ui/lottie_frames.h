#pragma once

#include <cstddef>
#include <cstdint>

#include "lvgl.h"

// Cache d'images pré-calculées des icônes Lottie de l'écran météo.
//
// Pourquoi : 7 Lottie rendues par ThorVG à 30 img/s coûtaient 75-98 % du
// cœur 0 (mesuré 2026-11-15, la grande seule = 45 %). Une boucle meteocons
// dure 6 s : on la rend UNE fois, à kFps, en RGB565 déjà composé sur la
// couleur (unie) de la carte, puis on ne fait plus que pointer l'image
// suivante. Le remplissage est paresseux — chaque image est rendue au moment
// où elle est montrée pour la première fois — si bien que le premier tour
// coûte ce que coûtait ThorVG à kFps, et les suivants presque rien.
//
// Les icônes de même source et même taille partagent une entrée (souvent
// plusieurs jours « nuageux »). Sans PSRAM suffisante, l'entrée tombe en
// rendu direct (une seule image réécrite) : jamais d'icône manquante.
//
// Tout se fait dans la tâche LVGL (ThorVG n'est pas réentrant et LVGL s'en
// sert) : mutex LVGL requis pour chaque appel.
namespace lottie_frames {

inline constexpr uint32_t kFps = 15;

struct Anim; // opaque

// nullptr si ThorVG refuse la source.
Anim *acquire(const uint8_t *data, size_t size, int32_t px, uint32_t bg_rgb);
void release(Anim *a);

// Image à montrer pour le top d'horloge `tick` (cadence kFps). `in_place` =
// même descripteur que la fois précédente mais pixels réécrits (rendu direct) :
// l'appelant doit invalider l'objet, lv_image_set_src ne le voyant pas changer.
const lv_image_dsc_t *frame(Anim *a, uint32_t tick, bool *in_place);

} // namespace lottie_frames
