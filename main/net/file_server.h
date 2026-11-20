#pragma once

// Serveur HTTP servant la carte microSD sur le réseau local.
//
// Éteint par défaut : toute IO sur la carte ajoute de la gigue au drain TCP du
// flux MJPEG, qui est le facteur limitant du débit vidéo. Un serveur permanent
// serait une porte ouverte en permanence sur ce chemin sensible.
//
// Aucun appel LVGL ici : ce module ne connaît pas l'interface.
namespace file_server {

// Sondes injectées : `net/` ne doit pas dépendre de `app/`. Chaque handler qui
// touche la carte les consulte et renvoie 503 si l'une est vraie.
struct Hooks {
  bool (*sd_busy)(); // session caméra, OTA en cours ou mode clé USB
  // Déclenche un OTA depuis un fichier de la carte. nullptr => l'endpoint
  // /api/ota répond 501. Injecté pour la même raison que sd_busy : `net/` ne
  // doit pas inclure sys/ota_ctrl.h.
  void (*flash_firmware)(const char *path);
};

// À appeler une fois au boot, avant tout start(). Ne démarre rien.
void init(const Hooks &hooks);

// Démarre le serveur. Idempotent : un second appel ne fait rien et renvoie
// true. Alloue le tampon de transfert en PSRAM.
bool start();

// Arrête le serveur et libère le tampon. Idempotent. httpd_stop() attend la
// fin des requêtes en cours.
void stop();

[[nodiscard]] bool running();

} // namespace file_server
