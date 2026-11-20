#pragma once

#include "lvgl.h"

// Onglet « Carte SD » de la page Micro/Debug : état de la carte, liste des
// fichiers journaux, visionneuse paginée, formatage.
//
// Contrainte structurante : la tâche LVGL ne doit JAMAIS toucher au système
// de fichiers. Un readdir ou un fread sur SD y bloque l'interface et, pendant
// une session, le rendu vidéo. Tous les accès disque passent par une tâche
// dédiée ; la tâche LVGL se contente de déposer une requête et de relever le
// résultat depuis un lv_timer.
namespace ui_sdlog {

// Construit le contenu de l'onglet dans `parent` (page d'onglet LVGL).
// Appelé depuis ui_mic::create_screen — mutex LVGL déjà tenu.
void create_tab(lv_obj_t *parent);

} // namespace ui_sdlog
