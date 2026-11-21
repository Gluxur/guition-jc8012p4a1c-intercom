#pragma once

#include "app/call_log_parse.h"

#include <cstddef>
#include <cstdint>

// Historique d'appels : trace chaque coup de sonnette, archive une capture de
// l'appel manqué sur la carte SD et tient le compteur d'appels non répondus que
// la page météo affiche.
//
// Ce que « appel » veut dire ici : un coup de sonnette (bouton « Sonner » de HA), et rien d'autre. Un
// accès manuel à la caméra depuis l'écran d'accueil passe par
// ui_camera_v2::open_screen_from_event() sans jamais toucher ce module — il
// n'est donc jamais tracé, ni compté comme manqué.
namespace call_history {

// Le compte à rebours de non-réponse doit pouvoir refermer l'écran caméra, et
// le module ne doit pas connaître camera_session pour autant : les deux points
// d'entrée sont injectés au boot, comme ui::Callbacks et le probe de streaming
// de video_mjpeg. C'est ce qui garde le graphe des modules app/ acyclique
// (camera_session appelle déjà on_camera_closing).
struct Hooks {
  bool (*camera_active)();
  void (*stop_camera)();
};

// Après sd_log::start() (carte montée) : charge le compteur depuis la NVS et
// crée le répertoire des captures.
void init(const Hooks &hooks);

// Un coup de sonnette est arrivé. Ré-armable : une re-sonnerie pendant que
// l'écran caméra est déjà ouvert relance le compte à rebours de non-réponse.
void on_call_start();

// L'utilisateur a décroché (bouton « Répondre »). L'appel ne sera pas compté
// comme manqué.
void on_answered();

// À appeler en tête du démontage de la session caméra, AVANT
// video_mjpeg::stop() : la mailbox JPEG doit encore contenir la dernière image.
// Écrit sur la carte SD (donc à n'appeler que depuis une tâche où bloquer est
// acceptable — la tâche cam_stop, pas LVGL).
// Renvoie true si l'appel était manqué, c'est-à-dire s'il faut basculer sur la
// page météo au lieu de revenir à l'accueil.
[[nodiscard]] bool on_camera_closing();

[[nodiscard]] uint32_t missed_count();

// Chemin complet de la capture du dernier appel manqué, "" si aucune.
[[nodiscard]] const char *last_shot_path();

// Remet le compteur à zéro (persisté). Appelé quand la capture est réellement
// ouverte, pas quand on ferme la notification : fermer la pastille ne veut pas
// dire qu'on a vu qui a sonné.
void acknowledge_missed();

// Relit la queue du journal et remplit `out` avec les `max` appels les plus
// récents, le plus récent en premier. Lecture SD bloquante : jamais depuis la
// tâche LVGL (cf. la tâche calls_load de ui_calls). Renvoie le nombre écrit,
// 0 si la carte est indisponible ou le journal vide.
size_t load_recent(call_log::Entry *out, size_t max);

} // namespace call_history
