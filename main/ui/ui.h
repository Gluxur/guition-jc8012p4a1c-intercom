#pragma once

#include <cstddef>
#include <cstdint>

namespace ui {

// Plain C function-pointer fields (`void (*start_camera)()`), not
// std::function<void()>. This is why main.cpp's lambdas assigned to these
// fields must be capture-less (see ui_cbs.mic_active in main.cpp) — a
// capturing lambda has no compatible function-pointer representation.
// A C# equivalent would use `Action`/`Func<bool>` delegates, which always
// support closures because the CLR delegate type itself carries a target
// object pointer; here that capability was deliberately traded away for a
// plain, no-allocation struct of raw pointers.
/* Hooks vers la logique applicative (main.cpp) : l'UI ne connaît pas
 * WebRTC, elle ne fait que déclencher. */
struct Callbacks {
  void (*stop_camera)();
  void (*start_camera_v2)(); // page caméra V2 (flux 720p plein écran)
  void (*start_mic)();
  void (*stop_mic)();
  bool (*mic_active)();
  // Ponts main.cpp -> page Micro/Debug (données produites par les tâches
  // réseau/mic, lues par le lv_timer de ui_mic). Voir ui_mic.cpp.
  uint8_t (*mic_level)();      // crête PCM 0-100
  uint8_t (*mic_conn_state)(); // bits[0..1]=état, bit[2]=réponse SDP reçue
  size_t (*log_drain)(char *dst, size_t cap);
  // Tonalité de test poussée vers la caméra (mixée au micro), bascule ON/OFF.
  void (*toggle_mic_tone)();
  bool (*mic_tone_active)();

  // Sonnerie jouée sur le haut-parleur de l'écran (page Micro/Debug). Teste
  // toute la chaîne locale — fichier chargé, gain, volume, play_pcm, codec —
  // sans passer par MQTT ni ouvrir de session caméra.
  void (*toggle_ringtone)();
  bool (*ringtone_active)();
};

/* Crée les écrans home + caméra (sliders volume/micro) et charge l'écran
 * home. Prend le mutex LVGL en interne. */
void create_screens(const Callbacks &cbs);

/* Label d'erreur plein écran sur la layer top LVGL. */
void init_error_ui();
void show_error(const char *msg);
void hide_error();

/* true tant qu'une erreur est affichée — lu par le pipeline vidéo pour
 * suspendre le rendu direct (sinon la frame écraserait le message). */
bool error_displayed();

/* L'écran caméra vit dans ui_camera_v2.{h,cpp} (namespace ui_camera_v2) :
 * video_zone_physical_rect / prepare_stream / update_stats / band_blit /
 * open_screen. */

/* Retour à l'écran home avec animation (utilisé par stop_camera). */
void load_home_screen();

/* Navigation pilotée de l'extérieur (Home Assistant) : « accueil », « meteo »,
 * « agenda », « appels », « configuration ». La caméra garde sa propre entité,
 * son ouverture lance une session. À appeler HORS tâche LVGL : prend le mutex
 * et POSTE l'ouverture à lvgl_task (lv_async_call) — la page est chargée au
 * tour LVGL suivant, pas au retour. false si le nom est inconnu ou le verrou
 * indisponible. */
[[nodiscard]] bool open_page(const char *name);

/* Nom de la page affichée (mêmes noms), sinon « autre » — caméra, debug,
 * ou mutex LVGL non obtenu en 200 ms. Home Assistant refuse un état de select
 * hors de ses options : d'où un seul nom de repli, déclaré comme option. */
[[nodiscard]] const char *current_page();

} // namespace ui
