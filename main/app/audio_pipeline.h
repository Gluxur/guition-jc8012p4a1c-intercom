#pragma once

#include <cstddef>
#include <cstdint>

namespace audio_pipeline {

// Lu à chaque frame pour savoir si un flux distant est en cours : la lecture
// est coupée hors streaming, et la transition streaming -> arrêt déclenche la
// purge du DMA I2S. Injecté (et non inclus depuis camera_session) pour garder
// le pipeline audio indépendant de la session WebRTC.
using StreamingProbe = bool (*)();

// Alloue les slots PCM (PSRAM), les deux files et démarre audio_render_task.
// false si un slot ou une file manque — l'audio est alors muet, le reste de
// l'application continue.
bool init(StreamingProbe is_streaming);

// Décode un chunk G.711 A-law et le met en file de lecture. Appelé depuis le
// thread réseau esp_peer : ne bloque jamais (0 tick), jette le chunk si la
// lecture I2S est en retard.
void submit_alaw(const uint8_t *alaw, size_t count);

// Réveille la tâche de lecture, bloquée en attente d'un slot distant hors
// streaming : sans ça la sonnerie n'est audible qu'au premier paquet WebRTC.
void wake();

} // namespace audio_pipeline
