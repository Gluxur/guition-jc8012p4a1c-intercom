#pragma once

// Page Configuration : saisie tactile des identifiants réseau (WiFi, hôte du
// serveur go2rtc / Home Assistant) enregistrés en NVS via `settings::`.
// Les valeurs saisies ne prennent effet qu'au redémarrage — cf. commentaire de
// settings::save().
#include "ui.h"

namespace ui_settings {

/* Construit l'écran. Appelée depuis ui::create_screens : mutex LVGL déjà tenu.
 * Les callbacks servent au bouton d'essai de la sonnerie, qui doit passer par
 * le même chemin que celui de la page Micro/Debug (armement + réveil du
 * pipeline audio), pas appeler ringtone:: en direct. */
void create_screen(const ui::Callbacks &cbs);

/* Ouvre l'écran depuis un callback d'événement LVGL (mutex déjà tenu) et
 * recharge les champs depuis la configuration effective. */
void open_screen_from_event();

/* Cet écran est-il l'écran chargé ? Mutex LVGL tenu par l'appelant. */
[[nodiscard]] bool is_active();

} // namespace ui_settings
