#pragma once

// Session caméra : la connexion esp_peer (audio G.711 descendant uniquement,
// la vidéo passe par le pipeline MJPEG) + son cycle de vie écran/tâches.
namespace camera_session {

// `audio_url` : flux WebRTC audio-only go2rtc. `mjpeg_url` : flux MJPEG HTTP
// de la page caméra (800x1280, plein écran). Les deux doivent rester valides
// pour toute la vie du programme (buffers statiques de settings, cf. règle
// CLAUDE.md sur les identifiants réseau).
// false si le canal de signalisation n'a pas pu être alloué.
bool init(const char *audio_url, const char *mjpeg_url);

// Ouvre le peer, prépare le framebuffer et démarre la vidéo MJPEG. Appelée
// depuis la tâche différée cam2_defer (ui_camera_v2), jamais depuis un
// callback LVGL : l'initialisation WebRTC est trop lourde pour le thread UI.
void start_hd();

// Coup de sonnette : fait sonner la platine de rue (mic_session::start_ringback)
// dès que le peer audio de cette session est connecté — tout de suite s'il l'est
// déjà. Appelable avant start_hd() : la demande attend la connexion, et seul
// stop() l'annule. Non bloquant, sûr depuis la tâche de l'API.
void request_ringback();

// Arrêt complet (vidéo, peer, purge audio) et retour à l'écran d'accueil.
// Appelable depuis un callback LVGL : rend la main IMMÉDIATEMENT et confie le
// démontage (1 à 7 s d'attentes de join) à la tâche cam_stop, qui reprend le
// mutex LVGL pour le retour à l'accueil. active() passe à false dès l'appel.
void stop();

// Lu par les pipelines audio/vidéo à chaque frame : hors streaming, plus aucune
// écriture dans le framebuffer DSI ni dans le codec.
[[nodiscard]] bool active();

} // namespace camera_session
