#pragma once

#include <cstdint>

// Backchannel micro : connexion esp_peer SÉPARÉE de celle de la caméra, dédiée
// à l'émission de l'audio local vers le VTO via go2rtc. Tout est préfixé mic_
// pour ne jamais confondre les deux connexions.
namespace mic_session {

// `url` : endpoint go2rtc du flux d'entrée audio (POST de l'offre SDP). Doit
// rester valide pour toute la vie du programme (littéral de main.cpp).
// false si le canal de signalisation n'a pas pu être alloué.
bool init(const char *url);

// Ouvre le peer et démarre les tâches d'émission. Appelé depuis un callback
// LVGL (mutex LVGL tenu). Ignoré si une session est active ou si un démontage
// est encore en vol.
void start();

// Ouvre le backchannel micro COUPÉ (des zéros à la place de la capture) avec le
// ding-dong synthétisé mixé par-dessus : le visiteur entend que ça sonne, pas la
// pièce. start() — le clic Répondre — démute et arrête la tonalité. Non
// bloquant, sûr hors LVGL. No-op si une session est déjà ouverte.
void start_ringback();

// Arrêt coopératif (jamais de vTaskDelete : cf. piège du sémaphore I2S) puis
// fermeture du peer. Appelable depuis un callback LVGL : rend la main
// IMMÉDIATEMENT (active() passe à false) et confie les joins, jusqu'à 2,5 s, à
// la tâche mic_stop. Tombé pendant une ouverture, il la fait refermer dès
// qu'elle aboutit.
void stop();

// Peer ouvert, coupé ou non. C'est l'état qui compte pour la SRAM et le SDIO
// (gardes OTA, watchdog réseau).
[[nodiscard]] bool active();

// L'utilisateur a décroché : peer ouvert ET capture transmise. C'est l'état du
// bouton Répondre — active() seul y serait vrai dès la sonnerie.
[[nodiscard]] bool live();

// Crête PCM normalisée 0-100 de la dernière trame capturée (VU-mètre).
[[nodiscard]] uint8_t level();

// bits[0..1] = état connexion (0 Idle, 1 Connexion, 2 Connecté, 3 Échec),
// bit[2] = réponse SDP go2rtc injectée.
[[nodiscard]] uint8_t conn_state();

} // namespace mic_session
