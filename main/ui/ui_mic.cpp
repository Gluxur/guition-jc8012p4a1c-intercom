#include "ui_mic.h"

#include <cstring>

#include "bsp.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "ui_sdlog.h"
#include "ui_stats.h"
#include "sys/i18n.h"
#include "theme.h"

using i18n::Str;

LV_FONT_DECLARE(montserrat_fr_36);

namespace ui_mic {

namespace {

ui::Callbacks s_cbs = {};
lv_obj_t *s_scr = nullptr;
lv_obj_t *s_tabs = nullptr;
lv_obj_t *s_state_lbl = nullptr;
lv_obj_t *s_sdp_lbl = nullptr;
lv_obj_t *s_gain_lbl = nullptr;
lv_obj_t *s_vu_bar = nullptr;
lv_obj_t *s_log_ta = nullptr;
lv_obj_t *s_tone_btn = nullptr;
lv_obj_t *s_tone_lbl = nullptr;
lv_obj_t *s_ring_btn = nullptr;
lv_obj_t *s_ring_lbl = nullptr;
lv_timer_t *s_timer = nullptr;

// Plafond de texte du panneau log : au-delà on retronque le début pour
// borner le coût de rendu LVGL.
constexpr uint32_t kLogMaxChars = 4000;

void cb_mic_gain_changed(lv_event_t *e) {
  lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
  // Persisté au relâché seulement (commit NVS = autre cœur gelé).
  bsp::Bsp::instance().set_mic_gain(
      static_cast<uint8_t>(lv_slider_get_value(slider)),
      lv_event_get_code(e) == LV_EVENT_RELEASED);
}

// Reflète l'état réel de la tonalité sur le bouton. Appelée au clic ET par le
// timer : la tonalité peut être coupée ailleurs (stop_mic_backchannel).
void sync_tone_btn() {
  if (!s_tone_btn || !s_cbs.mic_tone_active) {
    return;
  }
  bool on = s_cbs.mic_tone_active();
  lv_label_set_text_fmt(s_tone_lbl, "%s %s", on ? LV_SYMBOL_STOP : LV_SYMBOL_AUDIO,
                        on ? i18n::t(Str::kMicStopTune)
                           : i18n::t(Str::kMicTune));
  lv_obj_set_style_bg_color(s_tone_btn,
                            lv_color_hex(on ? ui::theme::active.btn_on : ui::theme::active.btn_neutral), 0);
}

// Relu par le timer de rafraîchissement : la sonnerie s'auto-désarme à 30 s
// (kMaxRingUs), le bouton doit repasser seul à « Sonnerie » sans qu'on y
// touche — contrairement à la tonalité micro, qui n'a pas de plafond.
void sync_ring_btn() {
  if (!s_ring_btn || !s_cbs.ringtone_active) {
    return;
  }
  const bool on = s_cbs.ringtone_active();
  lv_label_set_text_fmt(s_ring_lbl, "%s %s", on ? LV_SYMBOL_STOP : LV_SYMBOL_BELL,
                        on ? i18n::t(Str::kMicStopRing)
                           : i18n::t(Str::kMicRing));
  lv_obj_set_style_bg_color(s_ring_btn,
                            lv_color_hex(on ? ui::theme::active.btn_danger : ui::theme::active.btn_go), 0);
}

void cb_ring(lv_event_t *e) {
  (void)e;
  if (s_cbs.toggle_ringtone) {
    s_cbs.toggle_ringtone();
  }
  sync_ring_btn();
}

void cb_tone(lv_event_t *e) {
  (void)e;
  if (s_cbs.toggle_mic_tone) {
    s_cbs.toggle_mic_tone();
  }
  sync_tone_btn();
}

// Pause sur SCREEN_UNLOADED et pas seulement au bouton Retour : une sonnerie
// La sonnette ouvre la caméra depuis n'importe quel écran, et les timers de cette
// page continuaient alors de tourner pendant l'appel.
void cb_unloaded(lv_event_t *) {
  if (s_timer) {
    lv_timer_pause(s_timer);
  }
}

void cb_back(lv_event_t *e) {
  (void)e;
  if (s_timer) {
    lv_timer_pause(s_timer);
  }
  if (s_cbs.stop_mic) {
    s_cbs.stop_mic();
  }
  ui::load_home_screen();
}

// Rafraîchit bandeau + VU + logs. S'exécute dans la tâche LVGL (timer LVGL),
// donc appels lv_* autorisés. Draine les atomiques de mic_session et le ring de
// dbg_log via ui::Callbacks.
void refresh_timer(lv_timer_t *t) {
  (void)t;

  if (s_cbs.mic_conn_state) {
    uint8_t v = s_cbs.mic_conn_state();
    const char *txt = "Idle";
    switch (v & 0x03) {
    case 1:
      txt = i18n::t(Str::kMicConnecting);
      break;
    case 2:
      txt = i18n::t(Str::kMicConnected);
      break;
    case 3:
      txt = i18n::t(Str::kMicFailed);
      break;
    default:
      txt = "Idle";
      break;
    }
    lv_label_set_text_fmt(s_state_lbl, "Backchannel : %s", txt);
    lv_label_set_text(s_sdp_lbl, (v & 0x04)
                                     ? i18n::t(Str::kMicSdpYes)
                                     : i18n::t(Str::kMicSdpNo));
  }

  lv_label_set_text_fmt(s_gain_lbl, i18n::t(Str::kMicGainFmt),
                        (unsigned)bsp::Bsp::instance().mic_gain());

  if (s_cbs.mic_level) {
    lv_bar_set_value(s_vu_bar, s_cbs.mic_level(), LV_ANIM_OFF);
  }

  sync_tone_btn();
  // Ici et pas seulement dans cb_ring : la sonnerie s'auto-désarme à 30 s,
  // le bouton doit le refléter sans intervention.
  sync_ring_btn();

  if (s_cbs.log_drain) {
    char buf[512];
    size_t n = s_cbs.log_drain(buf, sizeof(buf) - 1);
    // Le drain tourne quel que soit l'onglet visible : le suspendre laisserait
    // le ring de dbg_log déborder, et l'onglet Micro repartirait avec un trou.
    // Seul le rendu du widget est conditionné à sa visibilité.
    if (n > 0 && s_tabs && lv_tabview_get_tab_active(s_tabs) == 0) {
      buf[n] = '\0';
      lv_textarea_add_text(s_log_ta, buf);
      // Borne le texte : retronque le début si trop long.
      const char *cur = lv_textarea_get_text(s_log_ta);
      size_t len = cur ? strlen(cur) : 0;
      if (len > kLogMaxChars) {
        // Copie d'abord : `cur` pointe DANS le texte du label, que set_text
        // libère avant de recopier sa source -> lecture après libération.
        // Tampon PSRAM alloué une fois (tâche LVGL uniquement) : 4 Ko en
        // `static` iraient en .bss, donc en SRAM interne, celle du DTLS.
        static char *tail = static_cast<char *>(
            heap_caps_malloc(kLogMaxChars + 1, MALLOC_CAP_SPIRAM));
        if (tail != nullptr) {
          memcpy(tail, cur + (len - kLogMaxChars), kLogMaxChars + 1);
          lv_textarea_set_text(s_log_ta, tail);
        }
      }
      lv_textarea_set_cursor_pos(s_log_ta, LV_TEXTAREA_CURSOR_LAST);
    }
  }
}

} // namespace

void create_screen(const ui::Callbacks &cbs) {
  s_cbs = cbs;

  s_scr = lv_obj_create(NULL);
  lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s_scr, lv_color_hex(ui::theme::active.dark_bg), 0);

  lv_obj_t *title = lv_label_create(s_scr);
  lv_label_set_text(title, LV_SYMBOL_AUDIO " Micro / Debug");
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_set_style_text_font(title, &montserrat_fr_36, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

  // --- Onglets ---
  // La page héberge deux vues (micro, carte SD). Les boutons Retour et
  // Mélodie restent HORS du tabview, enfants de s_scr : ils doivent rester
  // atteignables depuis les deux onglets.
  s_tabs = lv_tabview_create(s_scr);
  lv_tabview_set_tab_bar_size(s_tabs, 60);
  lv_obj_set_size(s_tabs, LV_PCT(100), 560);
  lv_obj_align(s_tabs, LV_ALIGN_TOP_MID, 0, 72);
  lv_obj_set_style_bg_color(s_tabs, lv_color_hex(ui::theme::active.dark_bg), 0);

  lv_obj_t *tab_mic = lv_tabview_add_tab(s_tabs, i18n::t(Str::kMicTab));
  lv_obj_t *tab_sd = lv_tabview_add_tab(s_tabs, i18n::t(Str::kMicSdTab));

  // --- Bandeau statut ---
  s_state_lbl = lv_label_create(tab_mic);
  lv_obj_set_style_text_color(s_state_lbl, lv_color_white(), 0);
  lv_label_set_text(s_state_lbl, "Backchannel : Idle");
  lv_obj_align(s_state_lbl, LV_ALIGN_TOP_LEFT, 8, 4);

  s_sdp_lbl = lv_label_create(tab_mic);
  lv_obj_set_style_text_color(s_sdp_lbl, lv_color_white(), 0);
  lv_label_set_text(s_sdp_lbl, i18n::t(Str::kMicSdpNo));
  lv_obj_align(s_sdp_lbl, LV_ALIGN_TOP_LEFT, 8, 40);

  s_gain_lbl = lv_label_create(tab_mic);
  lv_obj_set_style_text_color(s_gain_lbl, lv_color_white(), 0);
  lv_label_set_text(s_gain_lbl, i18n::t(Str::kMicGainZero));
  lv_obj_align(s_gain_lbl, LV_ALIGN_TOP_LEFT, 8, 76);

  // --- Slider de gain (vertical, colonne gauche) ---
  lv_obj_t *gain_slider = lv_slider_create(tab_mic);
  lv_obj_set_size(gain_slider, 40, 240);
  lv_obj_align(gain_slider, LV_ALIGN_LEFT_MID, 40, 30);
  lv_slider_set_range(gain_slider, 0, 100);
  lv_slider_set_value(gain_slider, bsp::Bsp::instance().mic_gain(),
                      LV_ANIM_OFF);
  lv_obj_add_event_cb(gain_slider, cb_mic_gain_changed, LV_EVENT_VALUE_CHANGED,
                      NULL);
  lv_obj_add_event_cb(gain_slider, cb_mic_gain_changed, LV_EVENT_RELEASED,
                      NULL);

  // --- VU-mètre (barre verticale) ---
  s_vu_bar = lv_bar_create(tab_mic);
  lv_obj_set_size(s_vu_bar, 40, 240);
  lv_obj_align(s_vu_bar, LV_ALIGN_LEFT_MID, 130, 30);
  lv_bar_set_range(s_vu_bar, 0, 100);
  lv_bar_set_value(s_vu_bar, 0, LV_ANIM_OFF);

  lv_obj_t *vu_cap = lv_label_create(tab_mic);
  lv_obj_set_style_text_color(vu_cap, lv_color_white(), 0);
  lv_label_set_text(vu_cap, i18n::t(Str::kMicLevel));
  lv_obj_align_to(vu_cap, s_vu_bar, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

  // --- Panneau log (textarea scrollable, lecture seule) ---
  s_log_ta = lv_textarea_create(tab_mic);
  lv_obj_set_size(s_log_ta, 700, 420);
  lv_obj_align(s_log_ta, LV_ALIGN_TOP_RIGHT, -12, 4);
  lv_obj_set_style_text_font(s_log_ta, LV_FONT_DEFAULT, 0);
  lv_textarea_set_text(s_log_ta, "");
  lv_obj_add_state(s_log_ta, LV_STATE_DISABLED); // lecture seule

  // --- Bouton retour ---
  lv_obj_t *back_btn = lv_button_create(s_scr);
  lv_obj_set_size(back_btn, 200, 80);
  lv_obj_align(back_btn, LV_ALIGN_BOTTOM_LEFT, 24, -24);
  lv_obj_set_style_bg_color(back_btn, lv_color_hex(ui::theme::active.btn_back), 0);
  lv_obj_set_style_shadow_width(back_btn, 0, 0); // cf. contrainte CPU, ui.cpp
  lv_obj_t *back_lbl = lv_label_create(back_btn);
  lv_label_set_text_fmt(back_lbl, "%s %s", LV_SYMBOL_LEFT, i18n::t(Str::kBack));
  lv_obj_set_style_text_font(back_lbl, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(back_lbl, lv_color_white(), 0);
  lv_obj_center(back_lbl);
  lv_obj_add_event_cb(back_btn, cb_back, LV_EVENT_CLICKED, NULL);

  // --- Bouton tonalité de test (à droite de Retour) ---
  s_tone_btn = lv_button_create(s_scr);
  lv_obj_set_size(s_tone_btn, 300, 80);
  lv_obj_align(s_tone_btn, LV_ALIGN_BOTTOM_LEFT, 248, -24);
  lv_obj_set_style_shadow_width(s_tone_btn, 0, 0); // cf. contrainte CPU, ui.cpp
  s_tone_lbl = lv_label_create(s_tone_btn);
  lv_obj_set_style_text_font(s_tone_lbl, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(s_tone_lbl, lv_color_white(), 0);
  lv_obj_center(s_tone_lbl);
  lv_obj_add_event_cb(s_tone_btn, cb_tone, LV_EVENT_CLICKED, NULL);
  sync_tone_btn();

  // --- Bouton d'écoute locale de la sonnerie (à droite de la tonalité) ---
  // Hors du tabview comme les deux autres : atteignable depuis les deux
  // onglets. Teste la chaîne audio locale sans Home Assistant ni session caméra.
  s_ring_btn = lv_button_create(s_scr);
  lv_obj_set_size(s_ring_btn, 300, 80);
  lv_obj_align(s_ring_btn, LV_ALIGN_BOTTOM_LEFT, 568, -24);
  lv_obj_set_style_shadow_width(s_ring_btn, 0, 0); // cf. contrainte CPU, ui.cpp
  s_ring_lbl = lv_label_create(s_ring_btn);
  lv_obj_set_style_text_font(s_ring_lbl, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(s_ring_lbl, lv_color_white(), 0);
  lv_obj_center(s_ring_lbl);
  lv_obj_add_event_cb(s_ring_btn, cb_ring, LV_EVENT_CLICKED, NULL);
  sync_ring_btn();

  ui_sdlog::create_tab(tab_sd);

  ui_stats::attach(s_scr); // overlay FPS/CPU/RAM (bas-droite)

  // Timer de rafraîchissement (200 ms), créé en pause : activé à l'ouverture.
  s_timer = lv_timer_create(refresh_timer, 200, NULL);
  lv_timer_pause(s_timer);
  lv_obj_add_event_cb(s_scr, cb_unloaded, LV_EVENT_SCREEN_UNLOADED, NULL);
}

void open_screen_from_event() {
  lv_scr_load(s_scr); // sans animation : cf. ui::load_home_screen()
  if (s_timer) {
    lv_timer_resume(s_timer);
  }
  // mic_session::start() défère lui-même l'ouverture du peer dans sa tâche
  // mic_start : rien à faire ici pour ne pas bloquer le dispatch LVGL.
  if (s_cbs.start_mic) {
    s_cbs.start_mic();
  }
}

void open_screen() {
  bsp::Bsp::LvglLock lvgl_lock;
  lv_scr_load(s_scr); // sans animation : cf. ui::load_home_screen()
  if (s_timer) {
    lv_timer_resume(s_timer);
  }
}

} // namespace ui_mic
