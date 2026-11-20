#pragma once

// Palette de l'interface, choisie au boot (prefs::ui_theme()).
//
// Pourquoi une palette lue à l'exécution plutôt que des constexpr : l'option
// « Thème » de la page Configuration doit pouvoir revenir à la palette
// d'origine sans recompiler. Les écrans gardent leurs noms locaux (kBg, kInk…)
// sous forme de RÉFÉRENCES vers les champs de `active` : une référence vers un
// membre d'objet statique est initialisée constamment (aucun problème d'ordre
// d'initialisation entre unités), et la valeur est relue à chaque usage.
//
// Les écrans sont construits une seule fois (ui::create_screens) : select()
// doit être appelé AVANT, un changement de palette ne vaut qu'au redémarrage.
//
// Aplats uniquement, ni dégradé ni ombre : contrainte CPU du panneau (un
// repaint plein écran coûte déjà ~52 ms de rotation PPA, cf. CLAUDE.md piège 9).
#include "sys/prefs.h"

#include <cstdint>

typedef struct _lv_obj_t lv_obj_t; // lv_types.h : évite lvgl.h dans ce header

namespace ui::theme {

struct Palette {
  // --- Écrans sombres (accueil, appels, configuration, micro, SD, USB) ---
  uint32_t dark_bg;    // fond d'écran
  uint32_t home_bg;    // fond de l'accueil
  uint32_t dark_row;   // ligne / carte sur fond sombre
  uint32_t dark_ink;   // texte principal sur fond sombre
  uint32_t dark_sub;   // texte secondaire sur fond sombre
  uint32_t dark_label; // libellé de formulaire
  uint32_t error_bg;   // bandeau d'erreur plein écran
  uint32_t error_edge; // bordure du bandeau d'erreur

  // --- Écrans « clairs » d'origine (météo, agenda, bandeau caméra) ---
  uint32_t bg;
  uint32_t card;
  uint32_t ink;
  uint32_t sub;
  uint32_t hair;     // filet, rail de slider
  uint32_t badge;    // fond de bouton au repos
  uint32_t warm;     // accent chaud (T° max, volume, jour courant)
  uint32_t cold;     // accent froid (T° min, micro)
  uint32_t today;    // fond du jour courant (agenda)
  uint32_t out_card; // jour hors du mois affiché
  uint32_t out_ink;
  uint32_t next_bg;  // pastille prochain événement (météo)
  uint32_t next_ink;
  // Dégradé vertical du fond de la page Météo : `bg` (haut) -> `bg_grad_mid`
  // -> `bg_grad` (bas). Sans coût par frame : toutes les icônes animées sont
  // posées sur des cartes opaques, LVGL ne repeint donc jamais le fond de
  // l'écran sous elles. 3 couleurs = CONFIG_LV_GRADIENT_MAX_STOPS.
  bool bg_grad_on;
  uint32_t bg_grad_mid;
  uint32_t bg_grad;

  // Cadre des cartes façon tableau de bord Home Assistant : coins plus
  // arrondis, bordure plus épaisse dans la couleur `hair`. false = chaque
  // écran garde son cadre d'origine (radius 6-8, filet 1 px ou rien).
  bool ha_cards;
  int32_t card_radius;
  int32_t card_border_w;

  // --- Sémantique ---
  uint32_t ok;         // lien établi, appel répondu
  uint32_t ko;         // lien absent, appel manqué
  uint32_t answer;     // bouton Répondre au repos
  uint32_t mic_on;     // micro ouvert
  uint32_t live_bg;    // pastille « En ligne »
  uint32_t status_ok;  // message de réussite
  uint32_t status_err; // message d'échec
  uint32_t warn;       // avertissement (fichier absent, USB débranché)
  uint32_t usb_up;     // hôte USB connecté

  // --- Boutons d'action ---
  uint32_t btn_back;
  uint32_t btn_save;
  uint32_t btn_debug;
  uint32_t btn_danger;
  uint32_t btn_go;
  uint32_t btn_neutral;
  uint32_t btn_on; // bascule active (page Micro)
  uint32_t btn_usb;
  uint32_t btn_fw;

  // --- Tuiles de l'accueil ---
  uint32_t tile_camera;
  uint32_t tile_weather;
  uint32_t tile_agenda;
  uint32_t tile_calls;
  uint32_t tile_settings;
  uint32_t tile_power;

  // --- Thème LVGL par défaut (interrupteurs, sliders, listes, onglets) ---
  uint32_t lv_primary;
  uint32_t lv_secondary;
};

// Palette en vigueur. Écrite une fois par select(), lue par tous les écrans.
extern Palette active;

// Copie la palette demandée dans `active` et ré-initialise le thème LVGL par
// défaut avec ses accents. Appelant : ui::create_screens(), mutex LVGL tenu.
void select(prefs::UiTheme theme);

// Pose le cadre `ha_cards` (rayon, épaisseur, couleur `hair`) sur une carte
// déjà construite. No-op quand le thème n'en a pas : le cadre d'origine reste.
// À appeler APRÈS le style propre de l'écran, qu'il remplace.
void card_frame(lv_obj_t *card);

// Pose le dégradé de fond `bg_grad_*` sur un écran. No-op sans dégradé.
void screen_gradient(lv_obj_t *scr);

} // namespace ui::theme
