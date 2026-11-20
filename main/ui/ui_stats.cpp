#include "ui_stats.h"

#include <cstdio>

#include "esp_timer.h"
#include "sys/prefs.h"
#include "sys/sys_stats.h"

namespace ui_stats {

namespace {

// Compteur de renders LVGL (incrémenté sur LV_EVENT_RENDER_READY de
// l'afficheur par défaut) et FPS dérivé. Tout vit dans la tâche LVGL
// (event cb, timers) : aucun verrou nécessaire.
uint32_t s_render_count = 0;
int s_fps = 0;
uint32_t s_fps_last_count = 0;
int64_t s_fps_last_us = 0;
bool s_inited = false;

void render_ready_cb(lv_event_t *e) {
  (void)e;
  s_render_count++;
}

// Timer global 1s : calcule le FPS à partir du delta de renders.
void fps_timer_cb(lv_timer_t *t) {
  (void)t;
  int64_t now = esp_timer_get_time();
  int64_t dt = now - s_fps_last_us;
  if (dt > 0) {
    s_fps = static_cast<int>((static_cast<int64_t>(s_render_count -
                                                   s_fps_last_count) *
                              1000000) /
                             dt);
  }
  s_fps_last_count = s_render_count;
  s_fps_last_us = now;
}

void init_once() {
  if (s_inited) {
    return;
  }
  lv_display_t *disp = lv_display_get_default();
  if (!disp) {
    return; // afficheur pas encore prêt : réessai au prochain appel
  }
  lv_display_add_event_cb(disp, render_ready_cb, LV_EVENT_RENDER_READY, NULL);
  s_fps_last_us = esp_timer_get_time();
  lv_timer_create(fps_timer_cb, 1000, NULL);
  s_inited = true;
}

// Overlay par écran : formate F/CPU/RAM depuis les globaux + sys_stats.
void overlay_timer_cb(lv_timer_t *t) {
  lv_obj_t *label = static_cast<lv_obj_t *>(lv_timer_get_user_data(t));
  if (!label) {
    return;
  }
  // Réglage relu ici et pas à l'attache : les écrans sont créés une fois au
  // boot, l'interrupteur de la page Configuration s'applique à chaud. Écrire
  // "" une seule fois suffit — repeindre un label identique invaliderait une
  // zone LVGL par seconde sur chaque écran.
  if (!prefs::show_stats()) {
    if (lv_label_get_text(label)[0] != '\0') {
      lv_label_set_text(label, "");
    }
    return;
  }
  sys_stats::CpuUsage cpu = sys_stats::cpu_usage_percent();
  char buf[32];
  snprintf(buf, sizeof(buf), "F%d Ca%d Cb%d R%d", s_fps, cpu.core_a, cpu.core_b,
           sys_stats::psram_used_percent());
  lv_label_set_text(label, buf);
}

} // namespace

void attach(lv_obj_t *screen) {
  if (!screen) {
    return;
  }
  init_once();

  lv_obj_t *label = lv_label_create(screen);
  lv_obj_add_flag(label, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_label_set_text(label, "");
  // Gris moyen : lisible sur fond clair (météo) comme sombre (accueil/mic).
  lv_obj_set_style_text_color(label, lv_color_hex(0x888888), 0);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
  lv_obj_align(label, LV_ALIGN_BOTTOM_RIGHT, -12, -6);

  lv_timer_create(overlay_timer_cb, 1000, label);
}

int lvgl_fps() {
  init_once();
  return s_fps;
}

} // namespace ui_stats
