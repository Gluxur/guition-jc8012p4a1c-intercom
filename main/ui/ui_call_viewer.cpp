#include "ui_call_viewer.h"

#include "bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "app/call_history.h"
#include "app/video_mjpeg.h"
#include "ui.h"
#include "ui_weather_v3.h"
#include "theme.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

LV_FONT_DECLARE(montserrat_fr_36);

namespace ui_call_viewer {
namespace {

constexpr const char *TAG = "ui_call_view";

// Palette du thème clair (identique à ui_weather_v3 / ui_camera_v2).
const uint32_t &kBadge = ui::theme::active.badge;
const uint32_t &kInk = ui::theme::active.ink;
const uint32_t &kHair = ui::theme::active.hair;

// Borne haute d'une capture : même taille qu'un slot de mailbox MJPEG, puisque
// c'est exactement une frame du flux qui a été archivée.
constexpr size_t kShotMax = 320 * 1024;

lv_obj_t *s_scr = nullptr;
lv_obj_t *s_back_btn = nullptr;

// Chemin de la capture à afficher, recopié par open_screen_from_event (tâche
// LVGL) et lu par la tâche d'affichage. Une seule visionneuse à la fois : la
// tâche est créée après l'écriture et personne d'autre n'y touche.
char s_path[80] = {};

// Écran à recharger en sortant. Écrit par open_screen_from_event (tâche LVGL),
// lu par cb_back (tâche LVGL) et par la tâche d'affichage sur échec.
void (*s_on_back)() = nullptr;

// Génération de visionnage, incrémentée à chaque ouverture ET à chaque Retour.
// view_task capture la sienne : un Retour pendant la lecture SD ou le décodage
// la rend périmée. Sans elle, l'image était présentée PAR-DESSUS l'écran
// rejoint (LVGL en PARTIAL ne repeint que l'invalidé), le compteur d'appels
// manqués était remis à zéro sans que la capture ait été vue, et
// release_still() libérait la sortie du décodeur sous son DMA.
std::atomic<uint32_t> s_gen{0};
// Une view_task vit : refus d'en lancer une seconde (même s_out, même client
// PPA, même present_frame), et c'est elle qui rend la sortie du décodeur.
std::atomic<bool> s_task_alive{false};

void go_back() {
  if (s_on_back != nullptr) {
    s_on_back();
  } else {
    ui_weather_v3::open_screen();
  }
}

// Repeint le bouton Retour PAR-DESSUS l'image.
//
// C'est le cœur du mécanisme : on ne passe PAS en mode direct. LVGL rend en
// mode PARTIAL et son flush vise `dsi_shown_frame_buffer()` — donc, une fois
// present_frame() fait, le buffer qui porte l'image. Invalider le seul bouton
// n'y repeint que sa bande : l'image reste, le bouton apparaît dessus, et ses
// changements d'état visuels continuent de fonctionner tout seuls. En mode
// direct, à l'inverse, le flush est un no-op et le bouton serait invisible.
void repaint_controls() {
  bsp::Bsp::LvglLock lvgl_lock;
  if (s_back_btn == nullptr) {
    return;
  }
  lv_obj_invalidate(s_back_btn);
  lv_refr_now(lv_display_get_default());
}

// Lit le fichier en PSRAM. Renvoie le buffer (à rendre par heap_caps_free) ou
// nullptr. Bloquant : jamais depuis la tâche LVGL.
uint8_t *read_shot(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (f == nullptr) {
    ESP_LOGE(TAG, "capture introuvable : %s", path);
    return nullptr;
  }
  uint8_t *buf =
      static_cast<uint8_t *>(heap_caps_malloc(kShotMax, MALLOC_CAP_SPIRAM));
  if (buf == nullptr) {
    fclose(f);
    ESP_LOGE(TAG, "alloc lecture (%u o) échouée", (unsigned)kShotMax);
    return nullptr;
  }
  const size_t got = fread(buf, 1, kShotMax, f);
  fclose(f);
  if (got == 0) {
    heap_caps_free(buf);
    ESP_LOGE(TAG, "capture vide : %s", path);
    return nullptr;
  }
  *out_len = got;
  return buf;
}

// ui::show_error n'a pas d'auto-effacement : posé depuis ici il resterait sur
// l'écran météo jusqu'à la prochaine session vidéo. Un timer LVGL non répétitif
// le retire — il tourne dans la tâche LVGL, donc sans verrou à prendre.
void cb_hide_error(lv_timer_t *t) {
  ui::hide_error();
  lv_timer_delete(t);
}

// Navigation postée à la tâche LVGL. Charger un écran depuis view_task y
// exécutait SCREEN_LOADED de façon synchrone — pour la Météo,
// update_events_locked() copie un gcal::EventList (~4,6 Ko) sur la pile : la
// pile de view_task débordait sur le chemin « Capture indisponible ».
void async_go_back(void *) {
  go_back();
  lv_timer_create(cb_hide_error, 3000, nullptr);
}

void async_invalidate(void *) { lv_obj_invalidate(lv_screen_active()); }

void view_task(void *arg) {
  const uint32_t my_gen =
      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
  auto mine = [&] { return s_gen.load() == my_gen; };
  // L'écran vient d'être chargé par l'appelant mais LVGL ne l'a pas encore
  // peint : le forcer maintenant, sinon le rendu tomberait APRÈS l'image et la
  // recouvrirait de noir.
  {
    bsp::Bsp::LvglLock lvgl_lock;
    lv_refr_now(lv_display_get_default());
  }

  size_t len = 0;
  uint8_t *jpeg = read_shot(s_path, &len);
  bool shown = false;
  if (jpeg != nullptr && mine()) {
    shown = video_mjpeg::render_still(jpeg, len);
  }
  heap_caps_free(jpeg);

  if (!mine()) {
    // Retour pressé pendant la lecture ou le décodage. Si l'image a été
    // présentée, elle recouvre l'écran rejoint : le faire repeindre en entier
    // (le flush vise le buffer affiché, donc celui de l'image).
    if (shown) {
      bsp::Bsp::LvglLock lvgl_lock;
      lv_async_call(async_invalidate, nullptr);
    }
    video_mjpeg::release_still();
    s_task_alive.store(false);
    vTaskDeleteWithCaps(nullptr);
    return;
  }

  if (!shown) {
    ESP_LOGE(TAG, "affichage de %s impossible", s_path);
    ui::show_error("Capture indisponible");
    // Bloc explicite : vTaskDelete() ne revient jamais et n'exécute donc AUCUN
    // destructeur de variable locale. Un LvglLock encore vivant partirait avec
    // le mutex LVGL pris à vie et figerait l'écran (cf. ui_calls::load_task).
    {
      bsp::Bsp::LvglLock lvgl_lock;
      lv_async_call(async_go_back, nullptr);
    }
    s_task_alive.store(false);
    vTaskDeleteWithCaps(nullptr);
    return;
  }

  repaint_controls();
  // Le compteur ne retombe à zéro qu'ici : fermer la notification sans regarder
  // la capture ne doit pas faire disparaître l'information.
  if (mine()) {
    call_history::acknowledge_missed();
  }
  s_task_alive.store(false);
  vTaskDeleteWithCaps(nullptr);
}

void cb_back(lv_event_t *) {
  s_gen.fetch_add(1);
  // ~2 Mo de sortie de décodeur rendus au tas : la PSRAM sert aux Lottie de
  // l'écran météo, qu'on rejoint à l'instant. Pas pendant qu'une view_task
  // décode encore dedans : c'est elle qui les rendra en sortant.
  if (!s_task_alive.load()) {
    video_mjpeg::release_still();
  }
  go_back();
}

} // namespace

void create_screen() {
  s_scr = lv_obj_create(NULL);
  lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(s_scr, 0, 0);

  s_back_btn = lv_button_create(s_scr);
  lv_obj_set_size(s_back_btn, 180, 72);
  lv_obj_align(s_back_btn, LV_ALIGN_TOP_LEFT, 24, 24);
  lv_obj_set_style_bg_color(s_back_btn, lv_color_hex(kBadge), 0);
  lv_obj_set_style_bg_opa(s_back_btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_back_btn, 16, 0);
  lv_obj_set_style_border_width(s_back_btn, 1, 0);
  lv_obj_set_style_border_color(s_back_btn, lv_color_hex(kHair), 0);
  lv_obj_set_style_shadow_width(s_back_btn, 0, 0);
  lv_obj_t *lbl = lv_label_create(s_back_btn);
  lv_label_set_text(lbl, LV_SYMBOL_LEFT " Retour");
  lv_obj_set_style_text_font(lbl, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(kInk), 0);
  lv_obj_center(lbl);
  lv_obj_add_event_cb(s_back_btn, cb_back, LV_EVENT_CLICKED, NULL);
}

void open_screen_from_event(const char *path, void (*on_back)()) {
  if (s_scr == nullptr || path == nullptr || path[0] == '\0') {
    ESP_LOGW(TAG, "ouverture ignorée : pas de capture à montrer");
    return;
  }
  if (s_task_alive.load()) {
    ESP_LOGW(TAG, "ouverture ignorée : affichage précédent encore en cours");
    return;
  }
  s_on_back = on_back;
  snprintf(s_path, sizeof(s_path), "%s", path);
  const uint32_t gen = s_gen.fetch_add(1) + 1;
  // Sans animation : lv_scr_load_anim repeindrait tout l'écran pendant les
  // 300 ms de transition, donc APRÈS que l'image ait été présentée.
  lv_scr_load(s_scr);
  // 8 Ko en PSRAM et non 4 Ko internes : lv_refr_now() fait tourner tout le
  // rendu logiciel LVGL sur cette pile. PSRAM sans risque depuis XIP_FROM_PSRAM
  // (plus de désactivation du cache pendant les accès flash).
  s_task_alive.store(true);
  if (xTaskCreateWithCaps(view_task, "call_view", 8192,
                          reinterpret_cast<void *>(static_cast<uintptr_t>(gen)),
                          4, nullptr, MALLOC_CAP_SPIRAM) != pdPASS) {
    s_task_alive.store(false);
    ESP_LOGE(TAG, "création call_view échouée : capture non affichée");
    go_back();
  }
}

} // namespace ui_call_viewer
