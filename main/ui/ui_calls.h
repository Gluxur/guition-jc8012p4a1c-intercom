#pragma once

// Page Appels : historique lu depuis /sdcard/calls.log, une ligne par coup de
// sonnette. Un appel manqué qui a laissé une capture est cliquable et ouvre la
// visionneuse (ui_call_viewer).
namespace ui_calls {

// À appeler une fois au boot, depuis ui::create_screens (mutex LVGL tenu).
void create_screen();

// Charge l'écran depuis un callback d'événement LVGL (mutex déjà tenu). La
// relecture du journal part dans une tâche dédiée : c'est de la lecture SD,
// elle n'a rien à faire sous le mutex LVGL. Sert aussi de cible de retour à la
// visionneuse.
void open_screen_from_event();

/* Cet écran est-il l'écran chargé ? Mutex LVGL tenu par l'appelant. */
[[nodiscard]] bool is_active();

} // namespace ui_calls
