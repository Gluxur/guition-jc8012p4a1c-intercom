#pragma once

#include <cstddef>
#include <cstdint>

namespace ota_ctrl {

// is_busy : renvoie true si une session vidéo/micro est active -> OTA refusé
//           (SRAM interne + bande passante SDIO partagée).
// report  : sink de statut ; main le câble sur l'entité « Statut OTA ».
// Crée une tâche dédiée qui attend une requête. À appeler une fois au boot.
void init(bool (*is_busy)(), void (*report)(const char *msg));

// Demande un OTA depuis `url`. Non bloquant : copie l'url + réveille la tâche.
// Sûr depuis la tâche de l'API. Ignore si un OTA est déjà en cours.
void request(const char *url);

// Demande un OTA depuis un fichier de la carte microSD. Même contrat que
// request() : non bloquant (copie le chemin + réveille la tâche), sûr depuis
// n'importe quel contexte, ignoré si un OTA est déjà en cours.
void request_file(const char *path);

// Métadonnées lues dans l'en-tête d'un .bin, pour que l'interface affiche ce
// qu'elle s'apprête à flasher.
struct ImageInfo {
  char version[32];
  char project[32];
  char date[16];
  uint32_t size_bytes;
};

// Lit l'en-tête de `path`. IO SD BLOQUANTE : jamais depuis la tâche LVGL —
// passer par la tâche disque de ui_sdlog. Renvoie false si le fichier est
// illisible ou n'est pas une image applicative ESP32.
[[nodiscard]] bool probe_file(const char *path, ImageInfo *out);

// Copie le dernier statut publié dans `out` ("ready" tant qu'aucun OTA n'a eu
// lieu depuis le démarrage). Le catalogue HA s'en sert comme getter : sans lui
// l'entité restait figée sur "reboot", dernier message envoyé avant
// esp_restart(), que plus personne ne remplaçait au boot suivant.
// Sûr depuis n'importe quelle tâche (copie sous verrou).
void status(char *out, size_t cap);

// True entre l'acceptation d'une requête OTA et la fin du téléchargement
// (succès ou échec). Sonde pure. sys/net_wdt s'en sert pour ne jamais
// redémarrer pendant l'écriture d'une partition.
[[nodiscard]] bool in_progress();

} // namespace ota_ctrl
