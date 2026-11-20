#pragma once

#include "ui.h"

namespace ui_agenda {

/* Crée l'écran Agenda (vue semaine/mois d'un agenda Google Calendar via Home
 * Assistant, cf. net/gcal.h). À appeler depuis ui::create_screens — mutex
 * LVGL déjà tenu. */
void create_screen();

/* Charge l'écran, remet l'ancrage sur la semaine courante (vue semaine) et
 * lance le premier fetch. Depuis un callback d'événement LVGL (mutex déjà
 * tenu). */
void open_screen_from_event();

/* Cet écran est-il l'écran chargé ? Mutex LVGL tenu par l'appelant. */
[[nodiscard]] bool is_active();

} // namespace ui_agenda
