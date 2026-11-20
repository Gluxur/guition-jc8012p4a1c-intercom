#pragma once

// Détection de présence par la caméra OV02C10 de la tablette.
//
// Le firmware ne fait QUE publier un état : c'est une automatisation Home
// Assistant qui décide d'éteindre ou rallumer l'écran (choix de l'utilisateur,
// 2026-11-15). Aucune image ne quitte la carte, à l'exception du cliché de
// diagnostic demandé explicitement (request_snapshot), écrit sur la SD locale.
//
// La caméra ne tourne que HORS session : pendant un appel, l'état est forcé à
// « présent » (quelqu'un répond ou la sonnette a retenti) et le CSI est coupé
// pour ne rien retirer au drain MJPEG ni à la SRAM interne du DTLS.
namespace presence {

struct Hooks {
  // Appelé depuis la tâche presence à chaque changement d'état. Injecté par
  // main.cpp vers ha_entities, pour garder presence -> ha_entities acyclique.
  void (*on_change)();
};

void init(const Hooks &hooks);

// Mouvement vu dans les kHoldMs dernières (ou session en cours).
[[nodiscard]] bool present();

// Écrit au prochain passage une vignette 322x182 en niveaux de gris dans
// /sdcard/presence.pgm (vérification de cadrage / exposition à distance).
void request_snapshot();

} // namespace presence
