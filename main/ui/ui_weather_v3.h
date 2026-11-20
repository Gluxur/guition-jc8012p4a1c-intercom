#pragma once

namespace ui_weather_v3 {

/* Crée l'écran météo V3 (ultra-minimaliste, thème clair). Widgets seulement,
 * pas de chargement réseau. À appeler depuis ui::create_screens — mutex LVGL
 * déjà tenu (appelé depuis ui::create_screens). */
void create_screen();

/* Charge l'écran V3 avec animation. À appeler depuis un callback d'événement
 * LVGL (mutex déjà tenu). */
void open_screen();

/* Cet écran est-il l'écran chargé ? Mutex LVGL tenu par l'appelant. */
[[nodiscard]] bool is_active();

} // namespace ui_weather_v3
