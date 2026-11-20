#pragma once

#include "ui.h"

namespace ui_mic {

// Crée l'écran Micro/Debug (bandeau statut, slider gain, VU-mètre, panneau
// log). À appeler depuis ui::create_screens — mutex LVGL déjà tenu.
void create_screen(const ui::Callbacks &cbs);

// Charge l'écran depuis un callback d'événement LVGL (mutex déjà tenu) et
// démarre le talkback micro (cbs.start_mic) via une tâche différée, pour
// éviter le deadlock LVGL au clic (cf. ui_camera::open_screen_from_event).
void open_screen_from_event();

// Charge l'écran depuis une tâche quelconque (prend le mutex LVGL en interne).
// Ne démarre PAS le talkback : sert au retour du mode clé USB, où l'on veut
// simplement retrouver la page, pas décrocher. Même dédoublement que
// ui_camera_v2::open_screen / open_screen_from_event.
void open_screen();

} // namespace ui_mic
