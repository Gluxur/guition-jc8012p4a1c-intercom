#include "ui.h"

#include "icons.h"
#include "ui_agenda.h"
#include "ui_call_viewer.h"
#include "ui_calls.h"
#include "ui_camera_v2.h"
#include "ui_mic.h"
#include "ui_settings.h"
#include "ui_stats.h"
#include "ui_usb.h"
#include "ui_weather_v3.h"

#include "bsp.h"
#include "lvgl.h"
#include "sys/prefs.h"
#include "net/wifi_net.h"
#include "net/esphome_api.h"
#include "theme.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>

LV_FONT_DECLARE(montserrat_fr_36);
LV_FONT_DECLARE(icons_fa_56);

namespace ui {

static Callbacks s_cbs = {};

static lv_obj_t *s_home_scr = NULL;
static lv_obj_t *s_error_label = NULL;

// Indicateurs de lien (mêmes codes couleur que la page Météo).
static const uint32_t &kNetOk = ui::theme::active.ok;    // lien établi (vert)
static const uint32_t &kNetKo = ui::theme::active.ko;    // lien absent (rouge)
static constexpr uint32_t kNetPeriodMs = 2 * 1000;
static lv_obj_t *s_net_wifi = NULL;
static lv_obj_t *s_net_ha = NULL;
static lv_timer_t *s_net_timer = NULL;
// Lu par la tâche de rendu vidéo (autre cœur) à chaque frame pour suspendre
// l'écriture directe dans le framebuffer : atomic, pas volatile — `volatile` en
// C++ n'impose que la relecture mémoire, sans ordonnancement.
static std::atomic<bool> s_error_displayed{false};

/* Contexte événement LVGL : mutex déjà tenu, pas de lock. */
static void cb_show_agenda(lv_event_t *e) {
  (void)e;
  ui_agenda::open_screen_from_event();
}

static void cb_show_camera_v2(lv_event_t *e) {
  (void)e;
  ui_camera_v2::open_screen_from_event();
}

static void cb_show_weather(lv_event_t *e) {
  (void)e;
  ui_weather_v3::open_screen();
}

/* La page Micro/Debug n'a plus de bouton d'accueil : c'est un outil de
 * diagnostic, il vit désormais dans la barre de la page Configuration. Sa place
 * ici est prise par l'historique d'appels. */
static void cb_show_calls(lv_event_t *e) {
  (void)e;
  ui_calls::open_screen_from_event();
}

static void cb_show_settings(lv_event_t *e) {
  (void)e;
  ui_settings::open_screen_from_event();
}

// Ce bouton coupe le RÉTROÉCLAIRAGE, il ne met plus l'appareil en veille
// profonde. Aucune source de réveil n'est armée sur cette carte : un deep sleep
// ne se quittait qu'en débranchant/rebranchant l'alimentation, ce qui oblige à
// démonter un écran mural pour rallumer. Le réveil au toucher
// (Bsp::handle_touch_read) rend l'extinction réversible du doigt.
//
// Rien à fermer, contrairement à l'ancienne version : l'appareil continue de
// tourner. Le micro reste ouvert s'il l'était (il ne l'est jamais depuis
// l'accueil), le journal SD continue d'écrire, la sonnette répond toujours —
// et un appel rallume l'écran de lui-même (camera_session::start_common).
//
// Le réveil au toucher reste ARMÉ ici : c'est tout l'intérêt.
static void cb_power_off(lv_event_t *e) {
  (void)e;
  bsp::Bsp::instance().backlight_off();
}

// Mutex LVGL requis (appelée depuis la tâche LVGL uniquement).
static void update_net_locked() {
  if (!s_net_wifi || !s_net_ha)
    return;
  // Réécrit seulement sur changement : un style local reposé à l'identique
  // invalide quand même la zone (lv_obj_set_local_style_prop ne compare pas),
  // soit un rendu + une rotation PPA toutes les 2 s pour rien. -1 = jamais
  // peint (premier appel, ou retour sur l'écran).
  static int s_wifi_shown = -1;
  static int s_ha_shown = -1;
  const int wifi = wifi_net::link_up() ? 1 : 0;
  const int ha = esphome_api::connected() ? 1 : 0;
  if (wifi != s_wifi_shown) {
    s_wifi_shown = wifi;
    lv_obj_set_style_text_color(s_net_wifi,
                                lv_color_hex(wifi ? kNetOk : kNetKo), 0);
  }
  if (ha != s_ha_shown) {
    s_ha_shown = ha;
    lv_obj_set_style_text_color(s_net_ha,
                                lv_color_hex(ha ? kNetOk : kNetKo), 0);
  }
}

static void net_timer_cb(lv_timer_t *) { update_net_locked(); }

/* Le sondage ne tourne que quand l'accueil est au premier plan : hors écran il
 * ne repeindrait rien et volerait du temps à la tâche LVGL pendant le
 * streaming. */
static void cb_home_screen_event(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) {
    update_net_locked();
    if (!s_net_timer)
      s_net_timer = lv_timer_create(net_timer_cb, kNetPeriodMs, nullptr);
  } else if (s_net_timer) {
    lv_timer_delete(s_net_timer);
    s_net_timer = nullptr;
  }
}

/* Tuiles de l'accueil : grille 3x2 sous le titre, dans les 1280x800 logiques.
 * Un seul constructeur pour que les six tuiles ne divergent pas à la prochaine
 * retouche. */
static constexpr int32_t kTileW = 378;
static constexpr int32_t kTileH = 300;
static constexpr int32_t kTileGap = 24;
static constexpr int32_t kTileLeft = 48;
static constexpr int32_t kTileTop = 128;

static lv_obj_t *make_home_tile(const char *icon, const char *text, int col,
                                int row, uint32_t bg, lv_event_cb_t cb) {
  lv_obj_t *btn = lv_button_create(s_home_scr);
  lv_obj_set_size(btn, kTileW, kTileH);
  lv_obj_set_pos(btn, kTileLeft + col * (kTileW + kTileGap),
                 kTileTop + row * (kTileH + kTileGap));
  lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
  lv_obj_set_style_radius(btn, 24, 0);
  lv_obj_set_style_pad_all(btn, 28, 0);
  /* Le thème par défaut pose une ombre floutée sur tout lv_button
   * (lv_theme_default.c : shadow_width LV_DPX(3), offset LV_DPX(4)) et le cache
   * de masques est désactivé (LV_DRAW_SW_SHADOW_CACHE_SIZE) : le flou est
   * recalculé à chaque dessin. Six grandes tuiles sur l'écran qu'on traverse
   * à CHAQUE transition — même raison qu'ailleurs (Météo, Agenda, Caméra). */
  lv_obj_set_style_shadow_width(btn, 0, 0);
  ui::theme::card_frame(btn);

  // Pastille d'icône : blanc translucide sur la couleur de la tuile, ce qui
  // évite une seconde teinte par tuile à maintenir.
  lv_obj_t *badge = lv_obj_create(btn);
  lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(badge, 104, 104);
  lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(badge, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
  lv_obj_set_style_border_width(badge, 0, 0);
  lv_obj_set_style_pad_all(badge, 0, 0);
  lv_obj_align(badge, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_t *ic = lv_label_create(badge);
  lv_label_set_text(ic, icon);
  lv_obj_set_style_text_font(ic, &icons_fa_56, 0);
  lv_obj_set_style_text_color(ic, lv_color_white(), 0);
  lv_obj_center(ic);

  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_align(lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
  return btn;
}

void create_screens(const Callbacks &cbs) {
  s_cbs = cbs;
  bsp::Bsp::LvglLock lvgl_lock;
  // Avant tout écran : chacun lit la palette en se construisant.
  ui::theme::select(prefs::ui_theme());

  ui_agenda::create_screen();
  ui_camera_v2::create_screen(s_cbs);
  ui_mic::create_screen(s_cbs);

  s_home_scr = lv_obj_create(NULL);
  lv_obj_remove_flag(s_home_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s_home_scr, lv_color_hex(ui::theme::active.home_bg), 0);

  lv_obj_t *title = lv_label_create(s_home_scr);
  lv_label_set_text(title, "Visiophone");
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

  /* Wi-Fi / Home Assistant en haut à droite, même sémantique que la page Météo. Conteneur
   * transparent : dans un flex, l'ordre de création est l'ordre d'affichage. */
  lv_obj_t *net_box = lv_obj_create(s_home_scr);
  lv_obj_remove_flag(net_box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(net_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(net_box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(net_box, 0, 0);
  lv_obj_set_style_pad_all(net_box, 0, 0);
  lv_obj_set_style_pad_column(net_box, 12, 0);
  lv_obj_set_flex_flow(net_box, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(net_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_align(net_box, LV_ALIGN_TOP_RIGHT, -32, 36);

  s_net_wifi = lv_label_create(net_box);
  lv_label_set_text(s_net_wifi, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_font(s_net_wifi, &lv_font_montserrat_24, 0);

  s_net_ha = lv_label_create(net_box);
  lv_label_set_text(s_net_ha, LV_SYMBOL_CHARGE);
  lv_obj_set_style_text_font(s_net_ha, &lv_font_montserrat_24, 0);

  update_net_locked();
  lv_obj_add_event_cb(s_home_scr, cb_home_screen_event, LV_EVENT_SCREEN_LOADED,
                      NULL);
  lv_obj_add_event_cb(s_home_scr, cb_home_screen_event,
                      LV_EVENT_SCREEN_UNLOADED, NULL);

  make_home_tile(icon::kVideo, "Caméra", 0, 0, ui::theme::active.tile_camera, cb_show_camera_v2);
  make_home_tile(icon::kCloudSun, "Météo", 1, 0, ui::theme::active.tile_weather, cb_show_weather);
  make_home_tile(icon::kCalendar, "Agenda", 2, 0, ui::theme::active.tile_agenda, cb_show_agenda);
  make_home_tile(LV_SYMBOL_CALL, "Appels", 0, 1, ui::theme::active.tile_calls, cb_show_calls);
  make_home_tile(LV_SYMBOL_SETTINGS, "Configuration", 1, 1, ui::theme::active.tile_settings,
                 cb_show_settings);
  make_home_tile(LV_SYMBOL_POWER, "Éteindre l'écran", 2, 1, ui::theme::active.tile_power,
                 cb_power_off);

  ui_stats::attach(s_home_scr); // overlay FPS/CPU/RAM (bas-droite)

  ui_weather_v3::create_screen();
  ui_settings::create_screen(s_cbs);
  ui_usb::create_screen();
  ui_call_viewer::create_screen();
  ui_calls::create_screen();

  lv_scr_load(s_home_scr);
}

void init_error_ui() {
  bsp::Bsp::LvglLock lvgl_lock;
  lv_obj_t *layer = lv_layer_top();
  s_error_label = lv_label_create(layer);
  lv_obj_set_width(s_error_label, lv_pct(90));
  lv_label_set_long_mode(s_error_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(s_error_label, LV_TEXT_ALIGN_CENTER, 0);
  // Bandeau et non texte nu : il s'affiche par-dessus la vidéo (ou par-dessus
  // ce que LVGL vient de repeindre), un texte sans fond y serait illisible.
  // montserrat_fr_36 est obligatoire : la police par défaut n'a pas les
  // accents français (« vidéo » sortirait troué).
  lv_obj_set_style_text_font(s_error_label, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(s_error_label, lv_color_white(), 0);
  lv_obj_set_style_bg_color(s_error_label, lv_color_hex(ui::theme::active.error_bg), 0);
  lv_obj_set_style_bg_opa(s_error_label, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(s_error_label, 24, 0);
  lv_obj_set_style_radius(s_error_label, 16, 0);
  lv_obj_set_style_border_width(s_error_label, 2, 0);
  lv_obj_set_style_border_color(s_error_label, lv_color_hex(ui::theme::active.error_edge), 0);
  lv_obj_set_style_border_opa(s_error_label, LV_OPA_COVER, 0);
  lv_obj_center(s_error_label);
  lv_obj_add_flag(s_error_label, LV_OBJ_FLAG_HIDDEN);
}

void show_error(const char *msg) {
  if (!s_error_label)
    return;
  s_error_displayed.store(true, std::memory_order_relaxed);
  bsp::Bsp::LvglLock lvgl_lock;
  bsp::Bsp::instance().set_direct_mode(false);
  lv_label_set_text(s_error_label, msg);
  lv_obj_clear_flag(s_error_label, LV_OBJ_FLAG_HIDDEN);
}

void hide_error() {
  if (!s_error_label)
    return;
  s_error_displayed.store(false, std::memory_order_relaxed);
  bsp::Bsp::LvglLock lvgl_lock;
  lv_obj_add_flag(s_error_label, LV_OBJ_FLAG_HIDDEN);
}

bool error_displayed() {
  return s_error_displayed.load(std::memory_order_relaxed);
}

/* Appelé depuis stop_camera(), lui-même déclenché par le bouton Retour —
 * donc toujours dans le contexte LVGL, mutex déjà tenu : pas de lock ici. */
void load_home_screen() {
  /* Sans animation, comme toutes les transitions de l'appli. Un repaint plein
   * cadre coûte ~52 ms rien qu'en rotation PPA (1024 kpx à 19,7 Mpx/s), plus le
   * rendu logiciel : un slide de 300 ms n'affichait donc que 3 à 6 étapes. Une
   * transition à 5 images/s se lit comme une panne, alors que le même
   * changement en un seul repaint se lit comme instantané. Même raisonnement
   * déjà appliqué dans ui_call_viewer. */
  lv_scr_load(s_home_scr);
}

namespace {
// Exécuté par lv_timer_handler, donc dans lvgl_task : mutex tenu, et SA pile
// (en PSRAM, dimensionnée pour ThorVG). Ouvrir l'agenda directement depuis la
// tâche de l'API faisait déborder la pile de celle-ci en construisant la grille
// de la semaine -> reset=PANIC (vécu 2026-11-15, BOOT #274).
void open_page_async(void *arg) {
  switch (reinterpret_cast<intptr_t>(arg)) {
  case 0:
    load_home_screen();
    break;
  case 1:
    ui_weather_v3::open_screen();
    break;
  case 2:
    ui_agenda::open_screen_from_event();
    break;
  case 3:
    ui_calls::open_screen_from_event();
    break;
  case 4:
    ui_settings::open_screen_from_event();
    break;
  default:
    break;
  }
}
} // namespace

bool open_page(const char *name) {
  static constexpr const char *kPages[] = {"accueil", "meteo", "agenda",
                                           "appels", "configuration"};
  intptr_t idx = -1;
  for (intptr_t i = 0; i < static_cast<intptr_t>(std::size(kPages)); i++) {
    if (strcmp(name, kPages[i]) == 0) {
      idx = i;
    }
  }
  if (idx < 0) {
    return false;
  }
  // Seul lv_async_call est fait ici, sous mutex (la liste des appels
  // différés de LVGL n'est pas protégée) : l'ouverture elle-même se fera au
  // prochain tour de lvgl_task. Délai borné : ne pas bloquer la tâche de l'API derrière
  // un repaint.
  bsp::Bsp::LvglLock lock(500);
  if (!lock.held()) {
    return false;
  }
  return lv_async_call(open_page_async, reinterpret_cast<void *>(idx)) ==
         LV_RESULT_OK;
}

const char *current_page() {
  bsp::Bsp::LvglLock lock(200);
  if (!lock.held()) {
    return "autre";
  }
  if (lv_screen_active() == s_home_scr) {
    return "accueil";
  }
  if (ui_weather_v3::is_active()) {
    return "meteo";
  }
  if (ui_agenda::is_active()) {
    return "agenda";
  }
  if (ui_calls::is_active()) {
    return "appels";
  }
  if (ui_settings::is_active()) {
    return "configuration";
  }
  return "autre";
}

} // namespace ui
