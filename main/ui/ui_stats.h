#pragma once

#include "lvgl.h"

namespace ui_stats {

// Attache un overlay stats (bas-droite) à `screen` : label + lv_timer 1s
// affichant "F<fps> Ca<> Cb<> R<>" (FPS LVGL global, CPU par cœur, PSRAM %).
// À appeler sous mutex LVGL (contexte de création d'écran). Le compteur FPS
// et son timer global sont installés une seule fois (lazy).
void attach(lv_obj_t *screen);

// FPS de rafraîchissement LVGL global (renders/s de l'afficheur par défaut),
// calculé par un timer interne. Auto-initialise le compteur au premier appel.
// Permet aux écrans possédant déjà leur propre overlay (ex. météo 7 jours) de
// préfixer le FPS sans réimplémenter la mesure.
int lvgl_fps();

} // namespace ui_stats
