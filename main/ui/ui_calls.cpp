#include "ui_calls.h"

#include "bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "app_config.h"
#include "app/call_history.h"
#include "ui.h"
#include "ui_call_viewer.h"
#include "theme.h"

#include <atomic>
#include <cstdint>

LV_FONT_DECLARE(montserrat_fr_24);
LV_FONT_DECLARE(montserrat_fr_36);

namespace ui_calls {
namespace {

constexpr const char *TAG = "ui_calls";

// Géométrie logique LVGL (1280x800 paysage), alignée sur ui_settings.
constexpr int32_t kScreenW = 1280;
constexpr int32_t kScreenH = 800;
constexpr int32_t kListY = 84;
constexpr int32_t kRowH = 76;

// Palette sombre des pages techniques (ui_settings / ui_mic).
const uint32_t &kBg = ui::theme::active.dark_bg;
const uint32_t &kRowBg = ui::theme::active.dark_row;
const uint32_t &kInk = ui::theme::active.dark_ink;
const uint32_t &kSub = ui::theme::active.dark_sub;
const uint32_t &kMissed = ui::theme::active.ko; // manqué (rouge)
const uint32_t &kAnswered = ui::theme::active.ok; // répondu (vert)

lv_obj_t *s_scr = nullptr;
lv_obj_t *s_list = nullptr;
lv_obj_t *s_status = nullptr;

// Entrées affichées, en PSRAM : ~7 Ko que la SRAM interne n'a pas à revendre.
// Écrites par la tâche calls_load, lues par les callbacks de clic (tâche LVGL).
// Pas de verrou : la liste est VIDÉE avant que la tâche parte, donc aucune ligne
// cliquable ne survit pendant la réécriture du tableau.
call_log::Entry *s_entries = nullptr;
size_t s_count = 0;

// Une seule relecture en vol : un double appui sur le bouton Appels ne doit pas
// lancer deux tâches qui reconstruiraient la liste en concurrence.
std::atomic<bool> s_loading{false};

void cb_row(lv_event_t *e) {
  const size_t idx =
      reinterpret_cast<size_t>(lv_event_get_user_data(e));
  if (s_entries == nullptr || idx >= s_count) {
    return;
  }
  ui_call_viewer::open_screen_from_event(s_entries[idx].path,
                                         open_screen_from_event);
}

// Mutex LVGL requis.
void add_row_locked(const call_log::Entry &e, size_t idx) {
  const bool has_shot = e.path[0] != '\0';

  lv_obj_t *row = lv_obj_create(s_list);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, kRowH);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(row, lv_color_hex(kRowBg), 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(row, 8, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  ui::theme::card_frame(row);

  lv_obj_t *stamp = lv_label_create(row);
  lv_label_set_text(stamp, e.stamp);
  lv_obj_set_style_text_font(stamp, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(stamp, lv_color_hex(kInk), 0);
  lv_obj_align(stamp, LV_ALIGN_LEFT_MID, 20, 0);

  lv_obj_t *verdict = lv_label_create(row);
  lv_label_set_text(verdict, e.missed ? "Manqué" : "Répondu");
  lv_obj_set_style_text_font(verdict, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(
      verdict, lv_color_hex(e.missed ? kMissed : kAnswered), 0);
  lv_obj_align(verdict, LV_ALIGN_LEFT_MID, 520, 0);

  lv_obj_t *hint = lv_label_create(row);
  lv_label_set_text(hint, has_shot ? LV_SYMBOL_IMAGE " Voir la photo"
                                   : "Pas de photo");
  lv_obj_set_style_text_font(hint, &montserrat_fr_24, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(has_shot ? kInk : kSub), 0);
  lv_obj_align(hint, LV_ALIGN_RIGHT_MID, -24, 0);

  // Seules les lignes qui ont réellement une capture réagissent : un appel
  // répondu, ou un manqué dont l'archivage a échoué, n'a rien à montrer et un
  // clic sans effet passerait pour un bouton mort.
  if (has_shot) {
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb_row, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(idx));
  } else {
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
  }
}

void load_task(void *arg) {
  (void)arg;
  const size_t n =
      call_history::load_recent(s_entries, (size_t)cfg::sd::kCallListMax);
  // Une ligne par ouverture de page, hors chemin chaud : c'est la seule trace
  // qui distingue « journal vide » de « lecture SD en échec » quand la liste
  // s'affiche vide. L'UART n'est pas branché en exploitation, mais elle part
  // aussi dans le journal SD et la page Debug.
  ESP_LOGI(TAG, "page Appels : %u entrée(s) relue(s)", (unsigned)n);

  // Bloc explicite : vTaskDelete() plus bas NE REVIENT JAMAIS, donc aucun
  // destructeur de variable locale ne s'exécute. Un LvglLock de portée fonction
  // partirait avec le mutex LVGL pris à vie — lvgl_task s'y bloque, l'écran
  // gèle et plus rien ne se repeint. Le verrou doit mourir AVANT la suppression.
  {
    bsp::Bsp::LvglLock lvgl_lock;
    s_count = n;
    lv_obj_clean(s_list); // repart d'une liste vide : cf. la note sur s_entries
    if (n == 0) {
      lv_label_set_text(s_status, "Aucun appel enregistré");
    } else {
      lv_label_set_text(s_status, "");
      for (size_t i = 0; i < n; i++) {
        add_row_locked(s_entries[i], i);
      }
    }
  }
  s_loading.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

void cb_back(lv_event_t *) { ui::load_home_screen(); }

} // namespace

void create_screen() {
  // Alloué une fois pour toutes : la page est rouverte souvent (retour depuis
  // la visionneuse), et 7 Ko de PSRAM rendus puis repris à chaque visite ne
  // feraient que fragmenter le tas.
  s_entries = static_cast<call_log::Entry *>(heap_caps_malloc(
      sizeof(call_log::Entry) * cfg::sd::kCallListMax, MALLOC_CAP_SPIRAM));
  if (s_entries == nullptr) {
    ESP_LOGE(TAG, "alloc du tableau d'appels échouée : page indisponible");
    return;
  }

  s_scr = lv_obj_create(nullptr);
  lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s_scr, lv_color_hex(kBg), 0);
  lv_obj_set_style_pad_all(s_scr, 0, 0);

  lv_obj_t *title = lv_label_create(s_scr);
  lv_label_set_text(title, LV_SYMBOL_CALL " Appels");
  lv_obj_set_style_text_font(title, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(kInk), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 18);

  s_status = lv_label_create(s_scr);
  lv_label_set_text(s_status, "");
  lv_obj_set_style_text_font(s_status, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(s_status, lv_color_hex(kSub), 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 22);

  lv_obj_t *back = lv_button_create(s_scr);
  lv_obj_set_size(back, 220, 56);
  lv_obj_align(back, LV_ALIGN_TOP_RIGHT, -24, 10);
  lv_obj_set_style_bg_color(back, lv_color_hex(ui::theme::active.btn_back), 0);
  lv_obj_set_style_shadow_width(back, 0, 0); // cf. contrainte CPU, ui.cpp
  lv_obj_t *back_lbl = lv_label_create(back);
  lv_label_set_text(back_lbl, "Retour");
  lv_obj_set_style_text_font(back_lbl, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(back_lbl, lv_color_white(), 0);
  lv_obj_center(back_lbl);
  lv_obj_add_event_cb(back, cb_back, LV_EVENT_CLICKED, nullptr);

  s_list = lv_obj_create(s_scr);
  lv_obj_set_pos(s_list, 20, kListY);
  lv_obj_set_size(s_list, kScreenW - 40, kScreenH - kListY - 12);
  lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_list, 0, 0);
  lv_obj_set_style_pad_all(s_list, 6, 0);
  lv_obj_set_style_pad_row(s_list, 8, 0);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_ACTIVE);
}

void open_screen_from_event() {
  if (s_scr == nullptr) {
    ESP_LOGE(TAG, "page Appels non construite : ouverture ignorée");
    return;
  }
  lv_scr_load(s_scr); // sans animation : cf. ui::load_home_screen()
  if (s_loading.exchange(true, std::memory_order_acq_rel)) {
    return; // relecture déjà en vol
  }
  // Vidée AVANT le départ de la tâche : plus aucune ligne cliquable ne pointe
  // sur s_entries pendant que celle-ci le réécrit.
  lv_obj_clean(s_list);
  s_count = 0;
  lv_label_set_text(s_status, "Chargement...");
  // Priorité 1, cœur 0 : même choix que la tâche disque de ui_sdlog — elle ne
  // dispute jamais le cœur au drain TCP du flux MJPEG, qui y tourne bien plus
  // haut. 6144 et non les 4096 de ui_sdlog : celle-ci enchaîne la lecture
  // FATFS/VFS ET la construction des lignes LVGL, la seconde s'ajoutant à la
  // profondeur de pile de la première.
  if (xTaskCreatePinnedToCore(load_task, "calls_load", 6144, nullptr, 1, nullptr,
                              0) != pdPASS) {
    ESP_LOGE(TAG, "création calls_load échouée : liste non chargée");
    lv_label_set_text(s_status, "Lecture du journal impossible");
    s_loading.store(false, std::memory_order_release);
  }
}

bool is_active() { return s_scr != nullptr && lv_screen_active() == s_scr; }

} // namespace ui_calls
