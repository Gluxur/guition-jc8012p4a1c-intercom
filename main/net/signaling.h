#pragma once

#include "esp_peer.h"

namespace signaling {

// Un canal = une connexion esp_peer en cours de négociation avec go2rtc.
// Chaque canal possède SON buffer de réponse : la connexion vidéo et le
// backchannel micro peuvent négocier en même temps, et un buffer global
// partagé créerait une race qu'aucun verrou inter-connexions ne couvre.
struct Channel {
  const char *tag = nullptr; // préfixe de log ("cam" / "mic")
  char *response = nullptr;  // corps de la réponse HTTP (PSRAM)
  int response_len = 0;
  int response_cap = 0;
  // Réponse plus grosse que le buffer : le chunk excédentaire est jeté. Sans
  // ce drapeau, exchange() injectait une answer SDP TRONQUÉE dans esp_peer et
  // renvoyait true — la panne se présentait alors comme un échec ICE, à des
  // heures de son vrai symptôme (buffer trop petit).
  bool overflow = false;
};

// Alloue le buffer de réponse en PSRAM (règle projet : la SRAM interne est
// réservée à la pile WebRTC, qui en consomme déjà ~71 Ko). false si l'alloc
// échoue — l'appelant doit alors renoncer à la connexion.
bool init_channel(Channel &ch, const char *tag);

// POST l'offre SDP locale sur `url` (endpoint /api/webrtc de go2rtc) ; l'answer
// reste dans ch.response. Ne touche à AUCUN peer : peut tourner hors de la
// tâche qui pilote esp_peer_main_loop. Bloquant, timeout 4 s par opération.
bool post(Channel &ch, const char *url, const char *local_sdp);

// Injecte l'answer de ch.response dans `peer` (esp_peer_send_msg). À appeler
// depuis la tâche qui fait tourner esp_peer_main_loop sur ce peer : le wrapper
// esp_peer n'a aucun verrou.
void inject(Channel &ch, esp_peer_handle_t peer);

// post() puis inject() : pour l'appel fait DANS esp_peer_main_loop (on_msg).
bool exchange(Channel &ch, esp_peer_handle_t peer, const char *url,
              const char *local_sdp);

} // namespace signaling
