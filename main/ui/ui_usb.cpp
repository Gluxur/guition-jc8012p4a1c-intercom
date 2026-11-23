#include "ui_usb.h"

#include "bsp.h"
#include "lvgl.h"
#include "sys/usb_msc.h"
#include "sys/i18n.h"
#include "theme.h"

using i18n::Str;

LV_FONT_DECLARE(montserrat_fr_36);
LV_FONT_DECLARE(montserrat_fr_24);

namespace ui_usb {
namespace {

lv_obj_t *s_scr = nullptr;
lv_obj_t *s_host_lbl = nullptr;
lv_timer_t *s_timer = nullptr;
void (*s_leave_cb)() = nullptr;

void cb_leave(lv_event_t *e) {
  (void)e;
  if (s_leave_cb != nullptr) {
    s_leave_cb();
  }
}

// 500 ms : l'état de l'hôte USB change au rythme d'un branchement humain.
// Aucun accès disque ici, uniquement la lecture d'un atomique.
void tick(lv_timer_t *t) {
  (void)t;
  if (s_host_lbl == nullptr) {
    return;
  }
  const bool up = usb_msc::host_connected();
  lv_label_set_text(s_host_lbl,
                    up ? i18n::t(Str::kUsbHostConnected)
                       : i18n::t(Str::kUsbHostNone));
  lv_obj_set_style_text_color(s_host_lbl,
                              lv_color_hex(up ? ui::theme::active.usb_up : ui::theme::active.warn), 0);
}

} // namespace

void create_screen() {
  s_scr = lv_obj_create(nullptr);
  lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s_scr, lv_color_hex(ui::theme::active.dark_bg), 0);

  lv_obj_t *title = lv_label_create(s_scr);
  lv_label_set_text(title, i18n::t(Str::kSdUsbModeTitle));
  lv_obj_set_style_text_font(title, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 80);

  lv_obj_t *info = lv_label_create(s_scr);
  // Le connecteur n'est pas serigraphie : ce libelle designe le port deduit,
  // a corriger si le premier essai montre que l'enumeration se produit
  // ailleurs.
  lv_label_set_text(info, i18n::t(Str::kUsbInfo));
  lv_obj_set_style_text_font(info, &montserrat_fr_24, 0);
  lv_obj_set_style_text_color(info, lv_color_hex(ui::theme::active.dark_label), 0);
  lv_obj_align(info, LV_ALIGN_CENTER, 0, -60);

  s_host_lbl = lv_label_create(s_scr);
  lv_label_set_text(s_host_lbl, i18n::t(Str::kUsbHostNone));
  lv_obj_set_style_text_font(s_host_lbl, &montserrat_fr_24, 0);
  lv_obj_set_style_text_color(s_host_lbl, lv_color_hex(ui::theme::active.warn), 0);
  lv_obj_align(s_host_lbl, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *warn = lv_label_create(s_scr);
  lv_label_set_text(warn, i18n::t(Str::kUsbDoorbellIgnored));
  lv_obj_set_style_text_font(warn, &montserrat_fr_24, 0);
  lv_obj_set_style_text_color(warn, lv_color_hex(ui::theme::active.dark_sub), 0);
  lv_obj_align(warn, LV_ALIGN_CENTER, 0, 50);

  lv_obj_t *btn = lv_button_create(s_scr);
  lv_obj_set_size(btn, 360, 90);
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, 160);
  lv_obj_set_style_bg_color(btn, lv_color_hex(ui::theme::active.btn_save), 0);
  lv_obj_set_style_shadow_width(btn, 0, 0); // cf. contrainte CPU, ui.cpp
  lv_obj_add_event_cb(btn, cb_leave, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, i18n::t(Str::kUsbFinish));
  lv_obj_set_style_text_font(lbl, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_center(lbl);

  s_timer = lv_timer_create(tick, 500, nullptr);
  lv_timer_pause(s_timer);
  // Relancé par show(), jamais remis en pause : après le premier mode clé USB
  // il tournait toutes les 500 ms jusqu'au redémarrage.
  lv_obj_add_event_cb(
      s_scr,
      [](lv_event_t *) {
        if (s_timer != nullptr) {
          lv_timer_pause(s_timer);
        }
      },
      LV_EVENT_SCREEN_UNLOADED, nullptr);
}

void show() {
  bsp::Bsp::LvglLock lvgl_lock;
  if (s_scr == nullptr) {
    return;
  }
  tick(nullptr); // état correct dès la première image
  if (s_timer != nullptr) {
    lv_timer_resume(s_timer);
  }
  lv_scr_load(s_scr);
}

void set_leave_cb(void (*cb)()) { s_leave_cb = cb; }

} // namespace ui_usb
