#pragma once

#include <cstddef>
#include <cstdint>

namespace video_mjpeg {

// Lu à chaque frame (réseau et rendu) : hors streaming, on ne touche plus au
// framebuffer DSI, que l'UI LVGL est en train de reprendre.
using StreamingProbe = bool (*)();

// Page écran consommatrice de la session. Pointeurs de fonctions nus (pas de
// std::function) : même convention que ui::Callbacks — aucune allocation, aucun
// état capturé, structure copiable telle quelle. Injectée pour que ce pipeline
// ne connaisse aucune page en particulier (deux écrans caméra le consomment).
struct View {
  // Rectangle physique cible (coords panneau), relu à chaque frame.
  void (*rect)(uint32_t &x, uint32_t &y, uint32_t &w, uint32_t &h);
  // Texte fps/cpu/psram, 1x/s. nullptr = pas de stats.
  void (*stats)(const char *text);
  // Superposition écrite dans le back-buffer courant après le décode et avant
  // present_frame(). nullptr quand la vidéo ne recouvre pas les contrôles.
  void (*overlay)();
  // Réarme les framebuffers après une interruption du flux : pendant le bandeau
  // « flux perdu », LVGL a repris la main et repeint l'écran dans le SEUL
  // buffer en cours de scan-out. Sans ce rappel, la reprise vidéo laisserait
  // l'autre buffer périmé (cf. docs/hardware-notes.md, deux framebuffers). Typiquement prepare_stream().
  void (*restore)();
};

// Crée le moteur JPEG matériel. À appeler TÔT au boot : il exige de la RAM
// interne DMA (descripteurs "rxlink"), rare après l'ouverture de WebRTC
// (~57 Ko libres) — d'où l'échec si on le crée dans start(). Réutilisable pour
// toutes les sessions caméra.
void init(StreamingProbe is_streaming);

// Alloue la mailbox (2 slots de `slot_bytes`), démarre la tâche de rendu puis
// le client réseau sur `url`. `slot_bytes` dépend de la résolution du flux :
// une frame plus grosse que le slot est abandonnée par le sink. `view` doit
// rester valide jusqu'au stop(). false si le moteur JPEG manque ou si une
// allocation échoue.
bool start(const char *url, size_t slot_bytes, const View &view);

// Arrête le réseau puis la tâche de rendu (arrêt coopératif) et libère la
// mailbox. Peut bloquer jusqu'à ~7 s (join du read réseau) : à appeler hors
// mutex LVGL.
void stop();

// Copie la dernière frame JPEG encore présente dans la mailbox vers `dst`
// (`cap` octets disponibles), pour l'archiver. À appeler AVANT stop() : celui-ci
// rend les slots au tas. false si aucune frame n'est disponible (mailbox vide,
// ou slot en cours d'écriture par la tâche réseau — réessayer alors quelques
// ms plus tard).
bool capture_snapshot(uint8_t *dst, size_t cap, size_t *out_len);

// Décode un JPEG isolé (une capture relue depuis la carte SD) et l'affiche sur
// TOUT le panneau, par le même chemin matériel que le flux live : décodeur JPEG
// HW + PPA + bascule de framebuffer. C'est la raison d'être de cette fonction —
// les captures sont en 800x1280, l'orientation PHYSIQUE du panneau, alors qu'un
// widget lv_image les afficherait dans l'espace LOGIQUE de LVGL (1280x800,
// tourné de 270°) donc couchées.
// Refuse de rendre pendant une session (le flux écraserait l'image aussitôt).
// Ne pose PAS le mode direct : l'appelant garde LVGL actif pour pouvoir
// repeindre ses propres contrôles par-dessus l'image (cf. ui_call_viewer).
// À appeler hors mutex LVGL — la bascule de framebuffer le prend elle-même.
bool render_still(const uint8_t *jpeg, size_t len);

// Rend au tas le buffer de sortie du décodeur alloué par render_still (jusqu'à
// ~2 Mo pour une image plein panneau). La PSRAM sert aux Lottie de l'écran
// météo dès qu'on quitte la visionneuse.
void release_still();

} // namespace video_mjpeg
