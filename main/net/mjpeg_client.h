#pragma once

#include <cstddef>
#include <cstdint>

// Client HTTP MJPEG (multipart/x-mixed-replace) pour le flux vidéo go2rtc
// décodé ensuite par le JPEG matériel du P4. Isolé du reste (règle CLAUDE.md :
// nouvelle responsabilité = module dédié). Le décodage/rendu ne vit PAS ici :
// ce module ne fait que récupérer le réseau et livrer des JPEG complets.
namespace mjpeg {

// Le client n'accumule PAS dans un buffer qui lui appartient : il écrit
// directement dans celui que le consommateur lui prête (slot de mailbox du
// rendu). Économise un memcpy de ~40-60 Ko par frame sur la tâche de drain TCP
// — mesuré comme le facteur limitant du débit (livraison-bound, cf. commentaire
// d'affinité de tâche dans start()) — et 256 Ko de PSRAM.
struct Sink {
  // Appelé au marqueur SOI. Renvoie le buffer d'accumulation et écrit sa
  // capacité utile dans `*cap`, ou nullptr pour sauter la frame.
  uint8_t *(*begin_frame)(size_t *cap, void *ctx);
  // Appelé au marqueur EOI : la frame complète occupe les `len` premiers
  // octets du buffer rendu par begin_frame.
  void (*end_frame)(size_t len, void *ctx);
  // Frame abandonnée (dépasse la capacité, ou flux coupé en cours de frame) :
  // le consommateur doit rendre le slot sans le publier.
  void (*abort_frame)(void *ctx);
  void *ctx;
};

// Démarre le client : crée une tâche dédiée qui GET le flux `url` en streaming,
// extrait les JPEG et les livre via `sink`. Reconnexion automatique si le flux
// tombe. Retourne false si la tâche n'a pas pu être créée, ou si un stop()
// précédent a échoué à joindre sa tâche (client définitivement abandonné :
// seul un redémarrage le remet en service). Idempotent : un second start sans
// stop est ignoré.
bool start(const char *url, const Sink &sink);

// Arrêt coopératif : lève le flag et attend la sortie réelle de la tâche
// (jamais de vTaskDelete brutal, cf. piège I2S). Sûr à appeler même si non
// démarré.
// false = la tâche n'était PAS sortie au bout de 7 s : elle peut encore
// appeler le sink. L'appelant ne doit alors libérer aucun buffer qu'elle
// touche (cf. video_mjpeg::stop, qui garde sa mailbox dans ce cas) — et ce
// pour TOUTE la durée de vie restante du firmware, pas seulement jusqu'au
// prochain start() : le client est marqué abandonné et ne redémarrera plus.
bool stop();

} // namespace mjpeg
