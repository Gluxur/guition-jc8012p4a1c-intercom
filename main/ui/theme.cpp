#include "theme.h"

#include "lvgl.h"

namespace ui::theme {

namespace {

// Valeurs historiques, reprises À L'IDENTIQUE des constantes qui vivaient dans
// chaque écran avant l'option « Thème » : choisir Classique doit rendre
// exactement l'interface d'avant.
constexpr Palette kClassic = {
    .dark_bg = 0x14141F,
    .home_bg = 0x141826,
    .dark_row = 0x1E2230,
    .dark_ink = 0xE6EAF2,
    .dark_sub = 0x8A93A6,
    .dark_label = 0xC8CEDA,
    .error_bg = 0x1D2333,
    .error_edge = 0xD97A3D,

    .bg = 0xE9EDF3,
    .card = 0xFCFCFA,
    .ink = 0x1D2333,
    .sub = 0x7A8296,
    .hair = 0xE3E7EE,
    .badge = 0xEFF2F6,
    .warm = 0xD97A3D,
    .cold = 0x3A7FC1,
    .today = 0xFDF3EA,
    .out_card = 0xF4F6F9,
    .out_ink = 0xA9B0BF,
    .next_bg = 0xF6C9A6,
    .next_ink = 0x6B3A17,
    .bg_grad_on = false,
    .bg_grad_mid = 0xE9EDF3,
    .bg_grad = 0xE9EDF3,
    .ha_cards = false,
    .card_radius = 8,
    .card_border_w = 1,

    .ok = 0x1FA85A,
    .ko = 0xE0451E,
    .answer = 0x1FA85A,
    .mic_on = 0xC0392B,
    .live_bg = 0xFDE3DF,
    .status_ok = 0x6FCF6F,
    .status_err = 0xE05C5C,
    .warn = 0xD97A3D,
    .usb_up = 0x7FD98F,

    .btn_back = 0x3E1616,
    .btn_save = 0x0E4D2A,
    .btn_debug = 0x2E2450,
    .btn_danger = 0x8E1B1B,
    .btn_go = 0x0B6B39,
    .btn_neutral = 0x16303E,
    .btn_on = 0x1B5E20,
    .btn_usb = 0x244055,
    .btn_fw = 0x2A4A2A,

    .tile_camera = 0x0B6B39,
    .tile_weather = 0xD97A3D,
    .tile_agenda = 0x2F6FB0,
    .tile_calls = 0x5B4A9E,
    .tile_settings = 0x34405A,
    .tile_power = 0x7A2020,

    .lv_primary = 0x1FA85A,
    .lv_secondary = 0xD97A3D,
};

// Coucher de soleil sur la côte : nuit violette, rose néon, orange, turquoise.
// Tous les écrans passent en sombre (météo et agenda compris) : un fond clair
// cassait l'ambiance, et le texte des écrans clairs (`ink`) devient clair.
constexpr Palette kVice = {
    .dark_bg = 0x1A0B2E,
    .home_bg = 0x1A0B2E,
    .dark_row = 0x2A1245,
    .dark_ink = 0xFFF4E8,
    .dark_sub = 0xB89BD0,
    .dark_label = 0xF0D6F5,
    .error_bg = 0x2A1245,
    .error_edge = 0xFF3E9A,

    .bg = 0x1A0B2E,
    .card = 0x2A1245,
    .ink = 0xFFF4E8,
    .sub = 0xC3A6DA,
    // Rose atténué : sert de bordure de carte (cadre ha_cards) et de filet.
    .hair = 0xB8367F,
    .badge = 0x3A1A5E,
    .warm = 0xFF9A3C,
    .cold = 0x2DE2E6,
    .today = 0x4D1A50,
    .out_card = 0x22103B,
    .out_ink = 0x7A6390,
    .next_bg = 0xFF3E9A,
    .next_ink = 0xFFFFFF,
    .bg_grad_on = true,
    .bg_grad_mid = 0x7A1F5C, // magenta
    .bg_grad = 0xE0663C,     // orange du soleil couchant, en bas d'écran
    .ha_cards = true,
    .card_radius = 14,
    .card_border_w = 2,

    .ok = 0x2DE2E6,
    .ko = 0xFF3E9A,
    .answer = 0x13B5B8,
    .mic_on = 0xFF3E9A,
    .live_bg = 0x4D1A50,
    .status_ok = 0x2DE2E6,
    .status_err = 0xFF5C8A,
    .warn = 0xFF9A3C,
    .usb_up = 0x2DE2E6,

    .btn_back = 0x7A1F4F,
    .btn_save = 0x0E7F82,
    .btn_debug = 0x5A2A9E,
    .btn_danger = 0xB3124F,
    .btn_go = 0x0E8A8C,
    .btn_neutral = 0x3B1F66,
    .btn_on = 0x0E8A8C,
    .btn_usb = 0x2B3F8F,
    .btn_fw = 0x1F6E6A,

    .tile_camera = 0xE8336F,
    .tile_weather = 0xF07A2A,
    .tile_agenda = 0x8A2BE2,
    .tile_calls = 0x0E9DA3,
    .tile_settings = 0x4A2A7A,
    .tile_power = 0x9E1B3B,

    .lv_primary = 0xFF3E9A,
    .lv_secondary = 0xFF9A3C,
};

} // namespace

Palette active = kClassic;

void select(prefs::UiTheme theme) {
  active = theme == prefs::UiTheme::kVice ? kVice : kClassic;
  // Mêmes paramètres que le BSP hormis les accents : mode sombre (pas d'ombre
  // floutée sur les boutons), police par défaut. En Classique l'appel est un
  // no-op, lv_theme_default_init() comparant ses paramètres avant de
  // reconstruire ses styles.
  lv_display_t *disp = lv_display_get_default();
  if (disp != nullptr) {
    lv_display_set_theme(
        disp, lv_theme_default_init(disp, lv_color_hex(active.lv_primary),
                                    lv_color_hex(active.lv_secondary),
                                    /*dark=*/true, LV_FONT_DEFAULT));
  }
}

void card_frame(lv_obj_t *card) {
  if (!active.ha_cards || card == nullptr) {
    return;
  }
  lv_obj_set_style_radius(card, active.card_radius, 0);
  lv_obj_set_style_border_width(card, active.card_border_w, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(active.hair), 0);
  lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
}

void screen_gradient(lv_obj_t *scr) {
  if (!active.bg_grad_on || scr == nullptr) {
    return;
  }
  // lv_obj_set_style_bg_grad garde le POINTEUR : le descripteur doit vivre
  // aussi longtemps que l'écran, d'où le stockage statique. Un seul écran
  // l'utilise ; un second partagerait le même dégradé, ce qui est voulu.
  static lv_grad_dsc_t s_grad;
  const lv_color_t colors[3] = {lv_color_hex(active.bg),
                                lv_color_hex(active.bg_grad_mid),
                                lv_color_hex(active.bg_grad)};
  // Le magenta tombe aux deux tiers : le haut (horloge, carte du jour) reste
  // sombre et lisible, l'orange n'apparaît que derrière la bande des 7 jours.
  const uint8_t fracs[3] = {0, 165, 255};
  lv_grad_init_stops(&s_grad, colors, nullptr, fracs, 3);
  lv_grad_vertical_init(&s_grad);
  lv_obj_set_style_bg_grad(scr, &s_grad, 0);
}

} // namespace ui::theme
