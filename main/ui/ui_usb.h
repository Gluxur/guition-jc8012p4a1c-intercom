#pragma once

// Écran plein cadre du mode clé USB. Bloquant au sens où il n'offre aucune
// navigation : le seul chemin de sortie est le bouton Terminer.
//
// L'écran n'appelle PAS usb_msc::leave() lui-même — la sortie démonte un
// système de fichiers et ne doit pas tourner dans la tâche LVGL. Il déclenche
// le rappel posé par ui_sdlog, qui le fait exécuter par sa tâche disque.
namespace ui_usb {

// Construit l'écran. À appeler depuis ui::create_screens — mutex LVGL tenu.
void create_screen();

// Charge l'écran. Sûr depuis n'importe quelle tâche (prend le mutex LVGL).
void show();

// Rappel déclenché par le bouton Terminer, exécuté dans le contexte LVGL :
// il doit se contenter de déposer une requête, jamais de faire le travail.
void set_leave_cb(void (*cb)());

} // namespace ui_usb
