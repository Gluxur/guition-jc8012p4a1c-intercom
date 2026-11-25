#include "ui_settings.h"

#include "bsp.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "media/ringtone.h"
#include "media/ringtone_sd.h"
#include "net/file_server.h"
#include "sys/i18n.h"
#include "sys/prefs.h"
#include "sys/sd_log.h"
#include "sys/settings.h"
#include "ui.h"
#include "ui_mic.h"
#include "theme.h"

#include <atomic>
#include <cstdio>
#include <cstring>

using i18n::Str;

LV_FONT_DECLARE(montserrat_fr_36);
// La police intégrée lv_font_montserrat_24 est générée sur -r 0x20-0x7F : ASCII
// seul, aucun caractère accentué. Sur cette page — libellés, champs de saisie
// ET clavier — ça mangeait tous les é/è/à/ç. La variante FR ajoute 0xA0-0xFF et
// 0x152-0x153 (œ), même jeu de glyphes que montserrat_fr_36.
LV_FONT_DECLARE(montserrat_fr_24);

namespace ui_settings {

namespace {

constexpr const char *TAG = "ui_settings";

// --- Géométrie (coordonnées logiques LVGL : 1280x800 paysage) --------------
constexpr int32_t kScreenW = 1280;
constexpr int32_t kScreenH = 800;
constexpr int32_t kBarH = 76;    // barre de titre + boutons
constexpr int32_t kFormY = 84;
constexpr int32_t kKeyboardH = 300;
constexpr int32_t kFormMargin = 12;
// Deux hauteurs : le formulaire tient entier sans clavier. Le figer à la
// hauteur « clavier ouvert » laissait 300 px vides en bas ET une barre de
// défilement permanente, le contenu (~650 px) dépassant les 404 px.
constexpr int32_t kFormH = kScreenH - kFormY - kFormMargin;
constexpr int32_t kFormHKeyboard = kFormH - kKeyboardH;
constexpr int32_t kLabelW = 300;
constexpr int32_t kRowH = 62;
constexpr int32_t kTokenRowH = 118; // jeton HA : ~250 caractères, multi-ligne
// Boîte de confirmation « enregistré » : les valeurs par défaut de
// lv_msgbox_class (260 px de large, bandeaux de 43 px) sont taillées pour la
// police 14 px du thème.
constexpr int32_t kMsgboxW = 620;
constexpr int32_t kMsgboxBandH = 64;

// Plancher du slider : à 0 % le panneau est noir, la page Configuration
// devient inatteignable et seul un effacement NVS récupère la carte.
constexpr int32_t kBacklightMin = 10;

lv_obj_t *s_scr = nullptr;
lv_obj_t *s_form = nullptr;
lv_obj_t *s_keyboard = nullptr;
lv_obj_t *s_status = nullptr;

lv_obj_t *s_ta_ssid = nullptr;
lv_obj_t *s_ta_wifi_pass = nullptr;
lv_obj_t *s_ta_host = nullptr;

lv_obj_t *s_slider_bl = nullptr;
lv_obj_t *s_lbl_bl = nullptr;
lv_obj_t *s_sw_dnd = nullptr;
lv_obj_t *s_sw_stats = nullptr;
lv_obj_t *s_sw_sdlog = nullptr;
lv_obj_t *s_sw_filesrv = nullptr;
lv_obj_t *s_lbl_filesrv = nullptr;
lv_obj_t *s_slider_ring = nullptr;
lv_obj_t *s_lbl_ring = nullptr;
lv_obj_t *s_lbl_ringfile = nullptr;
lv_obj_t *s_dd_ringfile = nullptr;
lv_obj_t *s_btn_ringtest = nullptr;
lv_obj_t *s_lbl_ringtest = nullptr;
lv_obj_t *s_dd_echo = nullptr;
lv_obj_t *s_dd_theme = nullptr;
lv_obj_t *s_dd_lang = nullptr;

// Ordre = valeurs de prefs::UiTheme : l'index du dropdown EST le réglage.
const char *theme_options() {
  return i18n::t(Str::kSettingsThemeOptions);
}
// Ordre = valeurs de prefs::EchoCancel : l'index du dropdown EST le réglage.
const char *echo_options() {
  return i18n::t(Str::kSettingsEchoOptions);
}
// Ordre = valeurs de i18n::Lang. Chaque langue écrite dans sa propre langue :
// c'est ce qu'on cherche des yeux quand l'interface est dans l'autre.
constexpr const char *kLangOptions = "Français\nEnglish";

// Callbacks applicatifs : seul le bouton d'essai de la sonnerie s'en sert.
ui::Callbacks s_cbs = {};

// Noms proposés par la liste déroulante, dans l'ordre affiché. L'entrée 0 est
// toujours la sonnerie interne et vaut le nom vide ; les suivantes sont les
// .wav trouvés à la racine. Conservé entre l'ouverture de l'écran et le clic :
// lv_dropdown ne rend qu'un index, et c'est le NOM qui part en NVS.
char s_ring_names[ringtone_sd::kMaxFiles + 1][ringtone_sd::kNameMax] = {};
size_t s_ring_count = 1;
// Un seul balayage de la racine SD en vol : deux tâches ring_scan écriraient le
// dropdown en concurrence, et la carte n'a qu'un contrôleur.
std::atomic<bool> s_ring_scanning{false};

const char *internal_ring_label() {
  return i18n::t(Str::kSettingsRingInternal);
}

// Le formulaire ne scrolle que lorsque le clavier lui prend 300 px.
void set_form_height(bool keyboard_open) {
  lv_obj_set_height(s_form, keyboard_open ? kFormHKeyboard : kFormH);
  if (!keyboard_open) {
    // Sans remise à zéro, l'offset de scroll hérité du mode réduit laisserait
    // le haut du formulaire hors vue alors que tout tient à l'écran.
    lv_obj_scroll_to_y(s_form, 0, LV_ANIM_OFF);
  }
}

// Le clavier ne s'affiche qu'à la prise de focus d'un champ : sinon il masque
// en permanence le bas du formulaire pour rien.
void cb_field_focused(lv_event_t *e) {
  lv_obj_t *ta = static_cast<lv_obj_t *>(lv_event_get_target(e));
  lv_keyboard_set_textarea(s_keyboard, ta);
  lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
  set_form_height(true);
  // Garde le champ visible dans la zone réduite.
  lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
}

// LV_EVENT_READY = touche « validation » du clavier, LV_EVENT_CANCEL = touche
// « fermer ». Les deux referment le clavier sans toucher au texte saisi.
void cb_keyboard_done(lv_event_t *e) {
  (void)e;
  lv_keyboard_set_textarea(s_keyboard, nullptr);
  lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
  set_form_height(false);
}

void hide_keyboard() {
  if (s_keyboard != nullptr) {
    lv_keyboard_set_textarea(s_keyboard, nullptr);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    set_form_height(false);
  }
}

// Copie bornée d'une textarea vers un champ de settings::Config. lv_textarea
// garantit déjà la longueur via set_max_length, mais la borne est réaffirmée
// ici : c'est cette copie qui protège le buffer NVS.
void read_field(char *dst, size_t cap, lv_obj_t *ta) {
  const char *txt = lv_textarea_get_text(ta);
  if (txt == nullptr) {
    txt = "";
  }
  strncpy(dst, txt, cap - 1);
  dst[cap - 1] = '\0';
}

void cb_reboot(lv_event_t *e) {
  (void)e;
  ESP_LOGI(TAG, "redémarrage demandé depuis la page Configuration");
  esp_restart();
}

void cb_close_msgbox(lv_event_t *e) {
  lv_obj_t *mbox = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
  lv_msgbox_close(mbox);
}

// lv_msgbox_class fixe une largeur de LV_DPI_DEF*2 (260 px) et des bandeaux
// haut/bas de dpi/3 (43 px), dimensionnés pour la police par défaut 14 px.
// En montserrat_fr_36 le titre et les boutons débordaient de cette boîte ;
// on garde la police accentuée la plus proche de la taille de référence
// (page SD/USB) et on redimensionne la boîte pour l'accueillir.
void show_reboot_box(const char *title_txt, const char *body) {
  lv_obj_t *mbox = lv_msgbox_create(nullptr);
  lv_obj_set_style_text_font(mbox, &montserrat_fr_24, 0);
  lv_obj_set_width(mbox, kMsgboxW);
  lv_obj_t *title = lv_msgbox_add_title(mbox, title_txt);
  lv_obj_set_height(lv_obj_get_parent(title), kMsgboxBandH);
  lv_msgbox_add_text(mbox, body);
  lv_obj_t *btn_now = lv_msgbox_add_footer_button(mbox, i18n::t(Str::kSettingsRestart));
  lv_obj_t *btn_later = lv_msgbox_add_footer_button(mbox, i18n::t(Str::kSettingsLater));
  lv_obj_set_height(lv_obj_get_parent(btn_now), kMsgboxBandH);
  lv_obj_add_event_cb(btn_now, cb_reboot, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(btn_later, cb_close_msgbox, LV_EVENT_CLICKED, mbox);
}

void cb_save(lv_event_t *e) {
  (void)e;
  hide_keyboard();

  // On repart de la configuration effective : un champ que l'écran n'expose
  // pas (aucun aujourd'hui) resterait ainsi intact au lieu d'être écrasé par
  // une chaîne vide.
  settings::Config cfg = settings::get();
  read_field(cfg.wifi_ssid, settings::kSsidMax, s_ta_ssid);
  read_field(cfg.wifi_pass, settings::kPassMax, s_ta_wifi_pass);
  read_field(cfg.host, settings::kHostMax, s_ta_host);

  // Un SSID ou un hôte vide rendrait la carte injoignable, et seul un
  // effacement NVS la récupérerait : on refuse avant d'écrire.
  if (cfg.wifi_ssid[0] == '\0' || cfg.host[0] == '\0') {
    lv_label_set_text(s_status, i18n::t(Str::kSettingsRequired));
    lv_obj_set_style_text_color(s_status, lv_color_hex(ui::theme::active.status_err), 0);
    return;
  }

  if (!settings::save(cfg)) {
    lv_label_set_text(s_status,
                      i18n::t(Str::kSettingsNvsFailed));
    lv_obj_set_style_text_color(s_status, lv_color_hex(ui::theme::active.status_err), 0);
    return;
  }
  lv_label_set_text(s_status, i18n::t(Str::kSettingsSaved));
  lv_obj_set_style_text_color(s_status, lv_color_hex(ui::theme::active.status_ok), 0);

  show_reboot_box(i18n::t(Str::kSettingsSavedTitle),
                  i18n::t(Str::kSettingsSavedBody));
}

// Les écrans sont construits une seule fois au boot avec la palette en
// vigueur : le thème choisi ne peut s'afficher qu'après un redémarrage.
void cb_theme(lv_event_t *e) {
  lv_obj_t *dd = static_cast<lv_obj_t *>(lv_event_get_target(e));
  const uint32_t sel = lv_dropdown_get_selected(dd);
  if (sel > static_cast<uint32_t>(prefs::UiTheme::kVice) ||
      static_cast<prefs::UiTheme>(sel) == prefs::ui_theme()) {
    return;
  }
  prefs::set_ui_theme(static_cast<prefs::UiTheme>(sel));
  show_reboot_box(i18n::t(Str::kSettingsThemeTitle),
                  i18n::t(Str::kSettingsThemeBody));
}

// Même contrat que le thème : écrans et entités HA sont libellés au boot
// (cf. sys/i18n.h). La boîte s'affiche dans la langue EN VIGUEUR, celle que
// l'utilisateur lit encore.
void cb_lang(lv_event_t *e) {
  lv_obj_t *dd = static_cast<lv_obj_t *>(lv_event_get_target(e));
  const uint32_t sel = lv_dropdown_get_selected(dd);
  if (sel > static_cast<uint32_t>(i18n::kLastLang) ||
      static_cast<i18n::Lang>(sel) == prefs::language()) {
    return;
  }
  prefs::set_language(static_cast<i18n::Lang>(sel));
  show_reboot_box(i18n::t(Str::kSettingsLangTitle),
                  i18n::t(Str::kSettingsLangBody));
}

void cb_back(lv_event_t *e) {
  (void)e;
  hide_keyboard();
  ui::load_home_screen();
}

// La page Micro/Debug est un outil de diagnostic, pas une fonction du
// visiophone : elle a quitté l'écran d'accueil (dont le bouton est devenu
// l'historique d'appels) pour la barre de cette page. open_screen_from_event
// démarre le talkback dans une tâche différée — sûr depuis ce contexte.
void cb_mic_debug(lv_event_t *e) {
  (void)e;
  hide_keyboard();
  ui_mic::open_screen_from_event();
}

// Bouton de la barre supérieure. Les boutons restent hors du formulaire
// scrollable et au-dessus du clavier : ils doivent être atteignables quel que
// soit l'état de la saisie.
lv_obj_t *make_bar_button(const char *text, int32_t x_offset, uint32_t bg,
                          lv_event_cb_t cb) {
  lv_obj_t *btn = lv_button_create(s_scr);
  lv_obj_set_size(btn, 220, 56);
  lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, x_offset, 10);
  lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
  lv_obj_set_style_shadow_width(btn, 0, 0); // cf. contrainte CPU, ui.cpp
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_center(lbl);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
  return btn;
}

// Pas d'ESP_LOG ici : LV_EVENT_VALUE_CHANGED part à chaque pixel de glissement,
// la saturation UART rendrait le slider inerte.
void cb_backlight(lv_event_t *e) {
  lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
  const int32_t v = lv_slider_get_value(slider);
  // Persisté au relâché seulement (commit NVS = autre cœur gelé).
  bsp::Bsp::instance().set_backlight(static_cast<uint8_t>(v),
                                     lv_event_get_code(e) == LV_EVENT_RELEASED);
  char txt[8]; // "100 %" + NUL
  snprintf(txt, sizeof txt, "%d %%", static_cast<int>(v));
  lv_label_set_text(s_lbl_bl, txt);
}

void cb_dnd(lv_event_t *e) {
  lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(e));
  prefs::set_dnd(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

void cb_show_stats(lv_event_t *e) {
  lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(e));
  prefs::set_show_stats(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

// L'écriture NVS et la coupure du journal sont toutes deux non bloquantes : la
// tâche sd_log ferme son fichier de son côté, au plus tard 200 ms après. Surtout
// pas de sd_log::flush_now() ici — il dort jusqu'à 500 ms, l'UI se figerait.
void cb_sd_log(lv_event_t *e) {
  lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(e));
  const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
  sd_log::set_enabled(on);
  prefs::set_sd_log_enabled(on);
}

// Sans cette ligne, l'utilisateur devrait aller chercher l'IP de l'appareil
// dans son routeur pour ouvrir la page.
void refresh_file_server_url() {
  if (s_lbl_filesrv == nullptr) {
    return;
  }
  if (!file_server::running()) {
    lv_label_set_text(s_lbl_filesrv, "");
    return;
  }
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip = {};
  if (netif == nullptr || esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
    lv_label_set_text(s_lbl_filesrv,
                      i18n::t(Str::kSettingsUnknownAddress));
    return;
  }
  char txt[32]; // "http://255.255.255.255/" + NUL
  snprintf(txt, sizeof txt, "http://" IPSTR "/", IP2STR(&ip.ip));
  lv_label_set_text(s_lbl_filesrv, txt);
}

// httpd_start/stop prennent quelques millisecondes : acceptable dans un
// callback tactile ponctuel, contrairement à un slider. La paire
// « appliquer puis persister » suit cb_sd_log : prefs ne pilote aucun module.
void cb_file_server(lv_event_t *e) {
  lv_obj_t *sw = static_cast<lv_obj_t *>(lv_event_get_target(e));
  const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
  if (on) {
    if (!file_server::start()) {
      // Démarrage refusé (PSRAM ou socket) : l'interrupteur doit revenir à
      // l'état réel, sinon il ment.
      lv_obj_remove_state(sw, LV_STATE_CHECKED);
      refresh_file_server_url();
      return;
    }
  } else {
    file_server::stop();
  }
  prefs::set_file_server(on);
  refresh_file_server_url();
}

// Pas d'ESP_LOG ici, même raison que cb_backlight : LV_EVENT_VALUE_CHANGED part
// à chaque pixel de glissement, la saturation UART rendrait le slider inerte.
void cb_ring_volume(lv_event_t *e) {
  lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
  const int32_t v = lv_slider_get_value(slider);
  ringtone::set_volume(static_cast<uint8_t>(v));
  char txt[8]; // "100 %" + NUL
  snprintf(txt, sizeof txt, "%d %%", static_cast<int>(v));
  lv_label_set_text(s_lbl_ring, txt);
}

// Persistance au RELÂCHEMENT seulement : une écriture NVS par pixel de
// glissement userait le flash et bloquerait la tâche LVGL sur le driver.
void cb_ring_volume_released(lv_event_t *e) {
  lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
  prefs::set_ring_volume(static_cast<uint8_t>(lv_slider_get_value(slider)));
}

void refresh_ringfile_label() {
  lv_label_set_text(s_lbl_ringfile, ringtone_sd::summary());
  const ringtone_sd::Status st = ringtone_sd::status();
  // kAbsent est ORANGE quand un fichier était demandé, GRIS quand l'utilisateur
  // a choisi la sonnerie interne : c'est le même statut pour deux situations
  // opposées, et seule la sélection courante les distingue.
  const bool internal = prefs::ring_file()[0] == '\0';
  const bool ok = internal || st == ringtone_sd::Status::kLoaded;
  lv_obj_set_style_text_color(
      s_lbl_ringfile, lv_color_hex(ok ? ui::theme::active.dark_sub : ui::theme::active.warn), 0);
}

// Reflète l'état réel de la sonnerie sur le bouton d'essai. Appelée au clic ET
// par le timer : elle s'auto-désarme à 30 s (kMaxRingUs), le bouton doit
// repasser seul à « Essai » sans qu'on y touche. Même motif que ui_mic.
void sync_ring_test_btn() {
  if (s_btn_ringtest == nullptr || s_cbs.ringtone_active == nullptr) {
    return;
  }
  const bool on = s_cbs.ringtone_active();
  lv_label_set_text_fmt(s_lbl_ringtest, "%s %s", on ? LV_SYMBOL_STOP : LV_SYMBOL_BELL,
                        on ? i18n::t(Str::kSettingsRingStop) : i18n::t(Str::kSettingsRingTest));
  lv_obj_set_style_bg_color(s_btn_ringtest,
                            lv_color_hex(on ? ui::theme::active.btn_danger : ui::theme::active.btn_go), 0);
}

void cb_ring_test(lv_event_t *e) {
  (void)e;
  if (s_cbs.toggle_ringtone) {
    s_cbs.toggle_ringtone();
  }
  sync_ring_test_btn();
}

// Le timer tourne en permanence (un lv_timer n'est pas attaché à un écran),
// d'où le filtre : hors de cette page, rien à rafraîchir.
void cb_ring_test_timer(lv_timer_t *t) {
  (void)t;
  if (lv_screen_active() == s_scr) {
    sync_ring_test_btn();
  }
}

// Le chargement lit jusqu'à 480 Ko de WAV sur la carte : quelques centaines de
// ms dans la tâche LVGL, sur un geste explicite et hors streaming. C'est le
// même appel que la sortie du mode clé USB, qui lui passe par une tâche disque
// parce qu'il démonte aussi le système de fichiers — ici il n'y a que la
// lecture, et une tâche de plus pour ça ne se justifie pas.
// Chargement hors tâche LVGL : ringtone_sd::load() lit jusqu'à ~480 Ko sur la
// SD, alloue en PSRAM et attend 50 ms (vTaskDelay) — un gel de l'UI de
// 100-300 ms à chaque choix de fichier quand il tournait dans le callback.
// Pile en PSRAM : sûr depuis XIP_FROM_PSRAM (le cache n'est plus coupé
// pendant les accès flash).
void ring_load_task(void *) {
  ringtone_sd::load();
  {
    // Bloc explicite : vTaskDeleteWithCaps ne déroule aucun destructeur.
    bsp::Bsp::LvglLock lvgl_lock;
    refresh_ringfile_label();
    sync_ring_test_btn(); // load() désarme la sonnerie : le bouton doit le dire
  }
  vTaskDeleteWithCaps(nullptr);
}

void cb_ring_file(lv_event_t *e) {
  lv_obj_t *dd = static_cast<lv_obj_t *>(lv_event_get_target(e));
  const uint32_t sel = lv_dropdown_get_selected(dd);
  if (sel >= s_ring_count) {
    return;
  }
  prefs::set_ring_file(s_ring_names[sel]);
  if (xTaskCreateWithCaps(ring_load_task, "ring_load", 4096, nullptr, 3,
                          nullptr, MALLOC_CAP_SPIRAM) != pdPASS) {
    ringtone_sd::load(); // repli : chargement sur place, comme avant
    refresh_ringfile_label();
    sync_ring_test_btn();
  }
}

void cb_echo(lv_event_t *e) {
  lv_obj_t *dd = static_cast<lv_obj_t *>(lv_event_get_target(e));
  const uint32_t sel = lv_dropdown_get_selected(dd);
  if (sel > static_cast<uint32_t>(prefs::EchoCancel::kHalfDuplex)) {
    return;
  }
  prefs::set_echo_cancel(static_cast<prefs::EchoCancel>(sel));
}

// Publie dans le dropdown la liste balayée par scan_ring_files_task.
// Mutex LVGL requis. `found` / `n` viennent du tampon de balayage : la copie
// vers s_ring_names se fait ICI, sous verrou, parce que cb_ring_file (tâche
// LVGL) indexe s_ring_names — le laisser réécrire par la tâche disque ferait
// pointer une sélection sur un nom à moitié copié.
void apply_ring_files_locked(const char found[][ringtone_sd::kNameMax],
                             size_t n) {
  s_ring_names[0][0] = '\0'; // entrée 0 : sonnerie interne, nom vide
  for (size_t i = 0; i < n; i++) {
    // memcpy et non snprintf("%s") : source et destination font exactement
    // kNameMax, et GCC ne sait pas borner un `const char (*)[kNameMax]` — il
    // refuse le snprintf en -Werror=format-truncation. La terminaison est
    // réimposée ici, list_wav ne stockant de toute façon que des noms plus
    // courts que kNameMax.
    memcpy(s_ring_names[1 + i], found[i], ringtone_sd::kNameMax);
    s_ring_names[1 + i][ringtone_sd::kNameMax - 1] = '\0';
  }
  s_ring_count = 1 + n;

  const char *cur = prefs::ring_file();
  size_t sel = 0;
  for (size_t i = 1; i < s_ring_count; i++) {
    if (strcmp(s_ring_names[i], cur) == 0) {
      sel = i;
      break;
    }
  }
  // Fichier configuré introuvable (effacé, ou carte absente) : on l'ajoute en
  // fin de liste au lieu de retomber en silence sur « interne ». La sélection
  // doit montrer ce qui est configuré, pas ce qui est disponible — et le label
  // d'état à côté dit déjà que le fichier manque.
  if (sel == 0 && cur[0] != '\0' && s_ring_count <= ringtone_sd::kMaxFiles) {
    snprintf(s_ring_names[s_ring_count], ringtone_sd::kNameMax, "%s", cur);
    sel = s_ring_count;
    s_ring_count++;
  }

  // Statique et non sur la pile : ~1 Ko, et la tâche LVGL n'en a pas à revendre.
  // lv_dropdown_set_options_static ne copie pas — le tampon doit donc survivre
  // à l'appel, ce qu'un tableau statique garantit.
  static char opts[(ringtone_sd::kMaxFiles + 2) * ringtone_sd::kNameMax];
  size_t off = 0;
  for (size_t i = 0; i < s_ring_count && off < sizeof(opts); i++) {
    const int written =
        snprintf(opts + off, sizeof(opts) - off, "%s%s", i == 0 ? "" : "\n",
                 i == 0 ? internal_ring_label() : s_ring_names[i]);
    if (written < 0) {
      break;
    }
    off += static_cast<size_t>(written);
  }
  lv_dropdown_set_options_static(s_dd_ringfile, opts);
  lv_dropdown_set_selected(s_dd_ringfile, static_cast<uint32_t>(sel));
}

// Balayage de la racine SD, HORS tâche LVGL. list_wav() fait un opendir + un
// readdir complet sur la FAT : appelé en ligne depuis open_screen_from_event()
// il retardait l'affichage entier de la page, mutex LVGL tenu, sur un
// périphérique dont le contrôleur est partagé avec la liaison Wi-Fi (cf.
// CLAUDE.md : aucune IO SD dans un chemin chaud).
// Priorité 1, cœur 0 : même choix que calls_load / ui_sdlog — elle ne dispute
// jamais le cœur au drain TCP du flux MJPEG, qui y tourne bien plus haut.
void scan_ring_files_task(void *arg) {
  (void)arg;
  // Tampon de travail local à la tâche : s_ring_names n'est écrit que sous
  // verrou LVGL, par apply_ring_files_locked. 1 Ko, d'où la pile généreuse.
  static char found[ringtone_sd::kMaxFiles][ringtone_sd::kNameMax];
  const size_t n = ringtone_sd::list_wav(found, ringtone_sd::kMaxFiles);

  // Bloc explicite : vTaskDelete() NE REVIENT JAMAIS, aucun destructeur de
  // variable locale ne s'exécute. Un LvglLock de portée fonction partirait avec
  // le mutex pris à vie — écran figé, logs qui continuent.
  {
    bsp::Bsp::LvglLock lvgl_lock;
    apply_ring_files_locked(found, n);
    refresh_ringfile_label();
  }
  s_ring_scanning.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

// Refait à CHAQUE ouverture de l'écran : le mode clé USB sert justement à
// déposer de nouveaux WAV, une liste figée à la construction les ignorerait.
void request_ring_rescan() {
  if (s_ring_scanning.exchange(true, std::memory_order_acq_rel)) {
    return; // balayage déjà en vol (double appui sur Configuration)
  }
  if (xTaskCreatePinnedToCore(scan_ring_files_task, "ring_scan", 4096, nullptr,
                              1, nullptr, 0) != pdPASS) {
    ESP_LOGE(TAG, "création ring_scan échouée : liste des sonneries non relue");
    s_ring_scanning.store(false, std::memory_order_release);
  }
}

// Conteneur d'une ligne du formulaire + son libellé à gauche. Partagé par les
// champs de saisie et par les réglages à application immédiate.
lv_obj_t *make_row(const char *label, int32_t height) {
  lv_obj_t *row = lv_obj_create(s_form);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, height);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);

  lv_obj_t *lbl = lv_label_create(row);
  lv_label_set_text(lbl, label);
  lv_obj_set_width(lbl, kLabelW);
  lv_obj_set_style_text_color(lbl, lv_color_hex(ui::theme::active.dark_label), 0);
  lv_obj_set_style_text_font(lbl, &montserrat_fr_24, 0);
  lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 4, 12);
  return row;
}

// Une ligne du formulaire : libellé fixe à gauche, saisie occupant le reste.
// `max_len` borne la saisie sur la capacité du champ NVS correspondant, `-1`
// pour le NUL : dépasser silencieusement puis tronquer à l'enregistrement
// donnerait une config différente de ce qui est affiché.
lv_obj_t *make_field(const char *label, size_t max_len, bool password,
                     bool multiline) {
  lv_obj_t *row = make_row(label, multiline ? kTokenRowH : kRowH);

  lv_obj_t *ta = lv_textarea_create(row);
  lv_obj_set_pos(ta, kLabelW, 0);
  lv_obj_set_size(ta, kScreenW - kLabelW - 90,
                  multiline ? kTokenRowH - 6 : kRowH - 6);
  lv_textarea_set_one_line(ta, !multiline);
  lv_textarea_set_max_length(ta, static_cast<uint32_t>(max_len - 1));
  lv_textarea_set_password_mode(ta, password);
  lv_obj_set_style_text_font(ta, &montserrat_fr_24, 0);
  lv_obj_add_event_cb(ta, cb_field_focused, LV_EVENT_FOCUSED, nullptr);
  return ta;
}

} // namespace

void create_screen(const ui::Callbacks &cbs) {
  s_cbs = cbs;
  s_scr = lv_obj_create(nullptr);
  lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s_scr, lv_color_hex(ui::theme::active.dark_bg), 0);
  lv_obj_set_style_pad_all(s_scr, 0, 0);

  lv_obj_t *title = lv_label_create(s_scr);
  lv_label_set_text_fmt(title, "%s %s", LV_SYMBOL_SETTINGS,
                        i18n::t(Str::kHomeSettings));
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_set_style_text_font(title, &montserrat_fr_36, 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 18);

  s_status = lv_label_create(s_scr);
  lv_label_set_text(s_status, "");
  lv_obj_set_style_text_font(s_status, &montserrat_fr_24, 0);
  lv_obj_set_style_text_color(s_status, lv_color_hex(ui::theme::active.dark_sub), 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 24, kBarH - 18);

  make_bar_button(i18n::t(Str::kBack), -24, ui::theme::active.btn_back, cb_back);
  make_bar_button(i18n::t(Str::kSettingsSave), -264, ui::theme::active.btn_save, cb_save);
  // Troisième bouton de la barre, à gauche des deux autres (les offsets sont
  // relatifs au bord droit et les boutons font 220 px + 20 de gouttière).
  make_bar_button(LV_SYMBOL_AUDIO " Micro / Debug", -504, ui::theme::active.btn_debug,
                  cb_mic_debug);

  s_form = lv_obj_create(s_scr);
  lv_obj_set_pos(s_form, 20, kFormY);
  lv_obj_set_size(s_form, kScreenW - 40, kFormH);
  lv_obj_set_style_bg_opa(s_form, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_form, 0, 0);
  lv_obj_set_style_pad_all(s_form, 6, 0);
  lv_obj_set_style_pad_row(s_form, 8, 0);
  lv_obj_set_flex_flow(s_form, LV_FLEX_FLOW_COLUMN);
  // Le formulaire dépasse la hauteur disponible dès que le clavier est ouvert :
  // scroll vertical seul (pas d'horizontal, qui ne ferait que décaler la vue).
  lv_obj_set_scroll_dir(s_form, LV_DIR_VER);
  // Le contenu dépasse désormais kFormH d'une vingtaine de pixels (ligne
  // « Volume sonnerie » ajoutée). Barre visible seulement pendant le
  // glissement : le mode permanent, lui, mangeait de la largeur en continu.
  lv_obj_set_scrollbar_mode(s_form, LV_SCROLLBAR_MODE_ACTIVE);

  // Réglages à effet immédiat : ils ne passent ni par settings::Config ni par
  // le bouton Enregistrer. Sans cette mention, l'utilisateur croit devoir
  // valider et redémarrer.
  lv_obj_t *hint = lv_label_create(s_form);
  lv_label_set_text(
      hint, i18n::t(Str::kSettingsImmediateHint));
  lv_obj_set_style_text_font(hint, &montserrat_fr_24, 0);
  lv_obj_set_style_text_color(hint, lv_color_hex(ui::theme::active.dark_sub), 0);

  lv_obj_t *row_bl = make_row(i18n::t(Str::kSettingsBrightness), kRowH);
  s_slider_bl = lv_slider_create(row_bl);
  lv_obj_set_pos(s_slider_bl, kLabelW, 20);
  lv_obj_set_width(s_slider_bl, kScreenW - kLabelW - 220);
  lv_slider_set_range(s_slider_bl, kBacklightMin, 100);
  lv_obj_add_event_cb(s_slider_bl, cb_backlight, LV_EVENT_VALUE_CHANGED,
                      nullptr);
  lv_obj_add_event_cb(s_slider_bl, cb_backlight, LV_EVENT_RELEASED, nullptr);

  s_lbl_bl = lv_label_create(row_bl);
  lv_obj_set_style_text_font(s_lbl_bl, &montserrat_fr_24, 0);
  lv_obj_set_style_text_color(s_lbl_bl, lv_color_hex(ui::theme::active.dark_label), 0);
  lv_label_set_text(s_lbl_bl, "");
  lv_obj_align(s_lbl_bl, LV_ALIGN_TOP_RIGHT, -16, 12);

  lv_obj_t *row_dnd = make_row(i18n::t(Str::kSettingsDnd), kRowH);
  s_sw_dnd = lv_switch_create(row_dnd);
  lv_obj_set_pos(s_sw_dnd, kLabelW, 8);
  lv_obj_set_size(s_sw_dnd, 90, 46);
  lv_obj_add_event_cb(s_sw_dnd, cb_dnd, LV_EVENT_VALUE_CHANGED, nullptr);

  // Overlay de diagnostic : masqué par défaut. Sur la page caméra il force une
  // recomposition du bandeau (mutex LVGL ~100 ms) donc une frame vidéo perdue à
  // chaque rafraîchissement. Effet sur l'écran caméra à la session suivante.
  // Posé sur la MÊME ligne que « Ne pas déranger » : une ligne de plus faisait
  // dépasser le formulaire des 704 px disponibles et sortait une barre de
  // défilement permanente.
  lv_obj_t *lbl_stats = lv_label_create(row_dnd);
  lv_label_set_text(lbl_stats, "Stats FPS/CPU/RAM");
  lv_obj_set_style_text_color(lbl_stats, lv_color_hex(ui::theme::active.dark_label), 0);
  lv_obj_set_style_text_font(lbl_stats, &montserrat_fr_24, 0);
  lv_obj_set_pos(lbl_stats, kLabelW + 160, 12);

  s_sw_stats = lv_switch_create(row_dnd);
  lv_obj_set_pos(s_sw_stats, kLabelW + 420, 8);
  lv_obj_set_size(s_sw_stats, 90, 46);
  lv_obj_add_event_cb(s_sw_stats, cb_show_stats, LV_EVENT_VALUE_CHANGED,
                      nullptr);

  // Troisième réglage sur cette même ligne, pour la raison ci-dessus : une
  // ligne de plus sortirait une barre de défilement permanente. La carte reste
  // montée quand l'interrupteur est éteint — seul le journal cesse d'écrire.
  lv_obj_t *lbl_sdlog = lv_label_create(row_dnd);
  lv_label_set_text(lbl_sdlog, i18n::t(Str::kSettingsSdLog));
  lv_obj_set_style_text_color(lbl_sdlog, lv_color_hex(ui::theme::active.dark_label), 0);
  lv_obj_set_style_text_font(lbl_sdlog, &montserrat_fr_24, 0);
  lv_obj_set_pos(lbl_sdlog, kLabelW + 560, 12);

  s_sw_sdlog = lv_switch_create(row_dnd);
  lv_obj_set_pos(s_sw_sdlog, kLabelW + 790, 8);
  lv_obj_set_size(s_sw_sdlog, 90, 46);
  lv_obj_add_event_cb(s_sw_sdlog, cb_sd_log, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *row_ring = make_row(i18n::t(Str::kSettingsRingVolume), kRowH);
  s_slider_ring = lv_slider_create(row_ring);
  lv_obj_set_pos(s_slider_ring, kLabelW, 20);
  lv_obj_set_width(s_slider_ring, 340);
  lv_slider_set_range(s_slider_ring, 0, 100);
  lv_obj_add_event_cb(s_slider_ring, cb_ring_volume, LV_EVENT_VALUE_CHANGED,
                      nullptr);
  lv_obj_add_event_cb(s_slider_ring, cb_ring_volume_released, LV_EVENT_RELEASED,
                      nullptr);

  s_lbl_ring = lv_label_create(row_ring);
  lv_obj_set_style_text_font(s_lbl_ring, &montserrat_fr_24, 0);
  lv_obj_set_style_text_color(s_lbl_ring, lv_color_hex(ui::theme::active.dark_label), 0);
  lv_label_set_text(s_lbl_ring, "");
  lv_obj_set_pos(s_lbl_ring, kLabelW + 360, 12);

  // État du fichier : sans ce retour, un ring.wav refusé est indiscernable d'un
  // ring.wav absent. L'UART n'est pas branché en exploitation, et une sonnerie
  // qui marche « mais pas la bonne » ne ressemble pas à une panne.
  s_lbl_ringfile = lv_label_create(row_ring);
  lv_obj_set_style_text_font(s_lbl_ringfile, &montserrat_fr_24, 0);
  lv_obj_set_style_text_color(s_lbl_ringfile, lv_color_hex(ui::theme::active.dark_sub), 0);
  lv_label_set_text(s_lbl_ringfile, "");
  lv_obj_set_pos(s_lbl_ringfile, kLabelW + 450, 12);

  // Ligne dédiée au choix du WAV : elle pousse le formulaire au-delà de la
  // hauteur visible, la barre de défilement est donc désormais attendue.
  lv_obj_t *row_file = make_row(i18n::t(Str::kSettingsRingFile), kRowH);

  s_dd_ringfile = lv_dropdown_create(row_file);
  lv_obj_set_pos(s_dd_ringfile, kLabelW, 4);
  lv_obj_set_size(s_dd_ringfile, 470, 52);
  lv_obj_set_style_text_font(s_dd_ringfile, &montserrat_fr_24, 0);
  // La liste déroulante est construite dès le constructeur du widget (elle est
  // rattachée à l'écran, pas au dropdown) : sa police se règle ici, une fois,
  // sans attendre la première ouverture.
  lv_obj_set_style_text_font(lv_dropdown_get_list(s_dd_ringfile),
                             &montserrat_fr_24, 0);
  lv_dropdown_set_options_static(s_dd_ringfile, internal_ring_label());
  lv_obj_add_event_cb(s_dd_ringfile, cb_ring_file, LV_EVENT_VALUE_CHANGED,
                      nullptr);

  // Le même essai que la page Micro/Debug, à portée de la liste : choisir un
  // fichier et devoir changer d'écran pour l'entendre rendait le réglage
  // inutilisable.
  s_btn_ringtest = lv_button_create(row_file);
  lv_obj_set_pos(s_btn_ringtest, kLabelW + 490, 4);
  lv_obj_set_size(s_btn_ringtest, 220, 52);
  lv_obj_set_style_shadow_width(s_btn_ringtest, 0, 0); // cf. ui.cpp
  s_lbl_ringtest = lv_label_create(s_btn_ringtest);
  lv_obj_set_style_text_font(s_lbl_ringtest, &montserrat_fr_24, 0);
  lv_obj_set_style_text_color(s_lbl_ringtest, lv_color_white(), 0);
  lv_obj_center(s_lbl_ringtest);
  lv_obj_add_event_cb(s_btn_ringtest, cb_ring_test, LV_EVENT_CLICKED, nullptr);
  sync_ring_test_btn();

  // Pris en compte au décroché suivant : l'AEC est créé une fois par appel,
  // dans la tâche micro, pas reconfiguré en pleine conversation.
  lv_obj_t *row_echo = make_row(i18n::t(Str::kSettingsEcho), kRowH);
  s_dd_echo = lv_dropdown_create(row_echo);
  lv_obj_set_pos(s_dd_echo, kLabelW, 4);
  lv_obj_set_size(s_dd_echo, 560, 52);
  lv_obj_set_style_text_font(s_dd_echo, &montserrat_fr_24, 0);
  lv_obj_set_style_text_font(lv_dropdown_get_list(s_dd_echo),
                             &montserrat_fr_24, 0);
  lv_dropdown_set_options_static(s_dd_echo, echo_options());
  lv_obj_add_event_cb(s_dd_echo, cb_echo, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *row_theme = make_row(i18n::t(Str::kSettingsTheme), kRowH);
  s_dd_theme = lv_dropdown_create(row_theme);
  lv_obj_set_pos(s_dd_theme, kLabelW, 4);
  lv_obj_set_size(s_dd_theme, 560, 52);
  lv_obj_set_style_text_font(s_dd_theme, &montserrat_fr_24, 0);
  lv_obj_set_style_text_font(lv_dropdown_get_list(s_dd_theme),
                             &montserrat_fr_24, 0);
  lv_dropdown_set_options_static(s_dd_theme, theme_options());
  lv_obj_add_event_cb(s_dd_theme, cb_theme, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *row_lang = make_row(i18n::t(Str::kSettingsLanguage), kRowH);
  s_dd_lang = lv_dropdown_create(row_lang);
  lv_obj_set_pos(s_dd_lang, kLabelW, 4);
  lv_obj_set_size(s_dd_lang, 560, 52);
  lv_obj_set_style_text_font(s_dd_lang, &montserrat_fr_24, 0);
  lv_obj_set_style_text_font(lv_dropdown_get_list(s_dd_lang),
                             &montserrat_fr_24, 0);
  lv_dropdown_set_options_static(s_dd_lang, kLangOptions);
  lv_obj_add_event_cb(s_dd_lang, cb_lang, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *row_srv = make_row(i18n::t(Str::kSettingsFileServer), kRowH);
  s_sw_filesrv = lv_switch_create(row_srv);
  lv_obj_set_pos(s_sw_filesrv, kLabelW, 8);
  lv_obj_set_size(s_sw_filesrv, 90, 46);
  lv_obj_add_event_cb(s_sw_filesrv, cb_file_server, LV_EVENT_VALUE_CHANGED,
                      nullptr);

  s_lbl_filesrv = lv_label_create(row_srv);
  lv_obj_set_style_text_font(s_lbl_filesrv, &montserrat_fr_24, 0);
  lv_obj_set_style_text_color(s_lbl_filesrv, lv_color_hex(ui::theme::active.dark_sub), 0);
  lv_obj_set_pos(s_lbl_filesrv, kLabelW + 120, 16);
  lv_label_set_text(s_lbl_filesrv, "");

  // 500 ms suffisent : le seul événement à rattraper est l'auto-désarmement à
  // 30 s. Le timer filtre sur l'écran actif, il ne coûte rien ailleurs.
  lv_timer_create(cb_ring_test_timer, 500, nullptr);

  s_ta_ssid = make_field("Wi-Fi SSID", settings::kSsidMax, false, false);
  s_ta_wifi_pass =
      make_field(i18n::t(Str::kSettingsWifiPassword), settings::kPassMax, true, false);
  // go2rtc + firmware served by Home Assistant. HA itself needs nothing here:
  // it discovers the board (ESPHome API over mDNS).
  s_ta_host = make_field(i18n::t(Str::kSettingsServer), settings::kHostMax, false,
                         false);

  // Créé en dernier : le clavier doit passer au-dessus du formulaire.
  s_keyboard = lv_keyboard_create(s_scr);
  lv_obj_set_size(s_keyboard, kScreenW, kKeyboardH);
  lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_text_font(s_keyboard, &montserrat_fr_24, 0);
  lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_textarea(s_keyboard, nullptr);
  lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(s_keyboard, cb_keyboard_done, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(s_keyboard, cb_keyboard_done, LV_EVENT_CANCEL, nullptr);
}

void open_screen_from_event() {
  // Recharge à chaque ouverture : la configuration effective est la seule
  // source de vérité, l'écran ne conserve pas d'état entre deux visites.
  const settings::Config &cfg = settings::get();
  // Seulement si le texte diffère (presque jamais) : avec max_length posé,
  // lv_textarea_set_text insère caractère par caractère — un événement, un
  // re-layout et, pour les mots de passe, une animation de masquage par
  // caractère. ~350 caractères à chaque ouverture, mutex LVGL tenu.
  auto set_if_changed = [](lv_obj_t *ta, const char *v) {
    if (strcmp(lv_textarea_get_text(ta), v) != 0) {
      lv_textarea_set_text(ta, v);
    }
  };
  set_if_changed(s_ta_ssid, cfg.wifi_ssid);
  set_if_changed(s_ta_wifi_pass, cfg.wifi_pass);
  set_if_changed(s_ta_host, cfg.host);

  const uint8_t bl = bsp::Bsp::instance().backlight();
  const int32_t bl_shown = bl < kBacklightMin ? kBacklightMin : bl;
  lv_slider_set_value(s_slider_bl, bl_shown, LV_ANIM_OFF);
  char bl_txt[8];
  snprintf(bl_txt, sizeof bl_txt, "%d %%", static_cast<int>(bl_shown));
  lv_label_set_text(s_lbl_bl, bl_txt);

  if (prefs::dnd()) {
    lv_obj_add_state(s_sw_dnd, LV_STATE_CHECKED);
  } else {
    lv_obj_remove_state(s_sw_dnd, LV_STATE_CHECKED);
  }

  if (prefs::show_stats()) {
    lv_obj_add_state(s_sw_stats, LV_STATE_CHECKED);
  } else {
    lv_obj_remove_state(s_sw_stats, LV_STATE_CHECKED);
  }

  // Reflet de l'état RÉEL du module, pas seulement de la préférence : un ring
  // non alloué (PSRAM épuisée au boot) laisse le journal éteint quoi qu'en dise
  // la NVS, et l'interrupteur doit le montrer.
  if (sd_log::enabled()) {
    lv_obj_add_state(s_sw_sdlog, LV_STATE_CHECKED);
  } else {
    lv_obj_remove_state(s_sw_sdlog, LV_STATE_CHECKED);
  }

  // Même principe que le journal SD : l'état RÉEL du module, pas la préférence.
  // Un httpd_start refusé au boot (PSRAM) doit se voir ici.
  if (file_server::running()) {
    lv_obj_add_state(s_sw_filesrv, LV_STATE_CHECKED);
  } else {
    lv_obj_remove_state(s_sw_filesrv, LV_STATE_CHECKED);
  }
  refresh_file_server_url();

  const uint8_t rv = prefs::ring_volume();
  lv_slider_set_value(s_slider_ring, rv, LV_ANIM_OFF);
  char rv_txt[8];
  snprintf(rv_txt, sizeof rv_txt, "%d %%", static_cast<int>(rv));
  lv_label_set_text(s_lbl_ring, rv_txt);

  // Relu à chaque ouverture plutôt que figé à la création : l'écran est
  // construit avant que ringtone_sd::load() ait forcément fini de s'exprimer
  // dans le journal, et le coût est nul.
  refresh_ringfile_label();
  lv_dropdown_set_selected(s_dd_echo,
                           static_cast<uint32_t>(prefs::echo_cancel()));
  lv_dropdown_set_selected(s_dd_theme,
                           static_cast<uint32_t>(prefs::ui_theme()));
  // La préférence et non i18n::lang() : après un changement sans redémarrage,
  // la liste doit montrer le choix enregistré, pas la langue encore affichée.
  lv_dropdown_set_selected(s_dd_lang,
                           static_cast<uint32_t>(prefs::language()));
  // Asynchrone : le balayage de la racine SD ne doit pas retarder l'affichage
  // de la page (cf. scan_ring_files_task). Le dropdown garde sa liste
  // précédente jusqu'au retour de la tâche — quelques dizaines de ms plus tard,
  // sur un écran déjà peint.
  request_ring_rescan();
  sync_ring_test_btn();

  lv_label_set_text(s_status, settings::provisioned()
                                  ? i18n::t(Str::kSettingsFromNvs)
                                  : i18n::t(Str::kSettingsDefaults));
  lv_obj_set_style_text_color(s_status, lv_color_hex(ui::theme::active.dark_sub), 0);

  hide_keyboard();
  lv_scr_load(s_scr); // sans animation : cf. ui::load_home_screen()
}

bool is_active() { return s_scr != nullptr && lv_screen_active() == s_scr; }

} // namespace ui_settings
