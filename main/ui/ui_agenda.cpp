#include "ui_agenda.h"

#include "bsp.h"
#include "lvgl.h"
#include "net/gcal.h"
#include "sys/i18n.h"
#include "theme.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

using i18n::Str;

LV_FONT_DECLARE(montserrat_fr_16);
LV_FONT_DECLARE(montserrat_fr_24);
LV_FONT_DECLARE(montserrat_fr_36);

namespace ui_agenda {

namespace {

constexpr uint32_t kRefreshPeriodMs = 15 * 60 * 1000;
constexpr uint32_t kPollPeriodMs = 200; // cf. s_refresh_pending ci-dessous

// Même palette claire que ui_weather_v3 (kBg/kCard/kInk/kSub/kHair/kWarm) :
// pages d'accueil sœurs, cohérence visuelle voulue.
const uint32_t &kBg = ui::theme::active.bg;
const uint32_t &kCard = ui::theme::active.card;
const uint32_t &kInk = ui::theme::active.ink;
const uint32_t &kSub = ui::theme::active.sub;
const uint32_t &kHair = ui::theme::active.hair;
const uint32_t &kBadge = ui::theme::active.badge;
const uint32_t &kWarm = ui::theme::active.warm;
const uint32_t &kToday = ui::theme::active.today; // fond crème du jour courant (kWarm dilué)
// Débords de grille (fin du mois précédent, début du suivant) : mêmes cellules,
// simplement en retrait. Un fond plus proche de kBg et une encre grise les
// mettent au second plan sans les faire disparaître.
const uint32_t &kOutCard = ui::theme::active.out_card;
const uint32_t &kOutInk = ui::theme::active.out_ink;

enum class Mode { kWeek, kMonth };


lv_obj_t *s_scr = nullptr;
lv_obj_t *s_range_lbl = nullptr;
lv_obj_t *s_mode_lbl = nullptr;
lv_obj_t *s_week_box = nullptr;
lv_obj_t *s_month_box = nullptr;
lv_timer_t *s_timer = nullptr;
lv_timer_t *s_poll_timer = nullptr;

// on_fetch_done tourne dans la tâche gcal_fetch, juste après la transaction
// HTTP (WiFi/SDIO) — jamais de mutation LVGL directe depuis là : le rendu
// (donc les transactions PPA du flush) est repoussé au prochain tick du
// timer LVGL, hors de toute activité SDIO récente. Découplage testé après
// des blocages PPA reproductibles ("PPA SRM sans fin de transaction") juste
// après un fetch.
std::atomic<bool> s_refresh_pending{false};

Mode s_mode = Mode::kWeek;
int64_t s_anchor_s = 0; // minuit local du jour de référence

// Vue semaine : 7 colonnes côte à côte (une par jour), pas 7 bandes empilées.
// L'écran est en paysage 1280x800 : la largeur est la ressource abondante, et
// des colonnes donnent une lecture "tableau" de la semaine.
struct WeekCol {
  lv_obj_t *card;   // fond de la colonne, teinté pour aujourd'hui
  lv_obj_t *day;    // "Lun 28"
  lv_obj_t *events; // lignes d'événements, une par ligne
};
WeekCol s_week_cols[7];

struct MonthCell {
  lv_obj_t *card;   // fond de la cellule, atténué hors du mois affiché
  lv_obj_t *num;
  lv_obj_t *events; // titres du jour en petit — remplace l'ancienne pastille
};
constexpr int kMonthCells = 42; // 6 semaines x 7 jours
MonthCell s_month_cells[kMonthCells];

// Plafonds d'affichage : au-delà, la colonne / la cellule déborde et LVGL
// coupe en "...". Les fixer ici évite de formater du texte jamais visible.
constexpr int kWeekMaxPerDay = 8;
constexpr int kMonthMaxPerDay = 3;

// Géométrie commune aux deux vues (écran logique 1280x800 paysage). La grille
// commence sous la barre de navigation et descend jusqu'à ~10 px du bas :
// c'est cette marge basse, inutilisée auparavant, qui agrandit les cellules.
constexpr int kGridY = 150;
constexpr int kGridH = 630;

int64_t local_midnight(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  lt.tm_hour = 0;
  lt.tm_min = 0;
  lt.tm_sec = 0;
  lt.tm_isdst = -1;
  return static_cast<int64_t>(mktime(&lt));
}

int64_t today_local_midnight() { return local_midnight(time(nullptr)); }

// Minuit local `n` jours après `midnight_s`. Le détour par midi puis
// renormalisation est nécessaire : les jours de changement d'heure durent 23 ou
// 25 h, et `+ n * 86400` y atterrit à 23:00 ou 01:00 — donc sur la mauvaise
// date civile, ce qui décalerait d'un jour la colonne des journées entières.
int64_t add_days_local(int64_t midnight_s, int n) {
  return local_midnight(
      static_cast<time_t>(midnight_s + static_cast<int64_t>(n) * 86400 + 43200));
}

// Minuit UTC de la même date civile que ce minuit local : ancre des événements
// "journée entière" (cf. gcal::events_on_day). Sans ça, en +02:00, un all-day
// d'un jour déborde de deux heures sur la colonne du lendemain.
int64_t utc_day_of(int64_t local_midnight_s) {
  time_t t = static_cast<time_t>(local_midnight_s);
  struct tm lt;
  localtime_r(&t, &lt);
  return gcal::utc_midnight(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}

// Lundi de la semaine contenant `anchor_s`.
int64_t week_start(int64_t anchor_s) {
  time_t t = static_cast<time_t>(anchor_s);
  struct tm lt;
  localtime_r(&t, &lt);
  int since_monday = (lt.tm_wday == 0) ? 6 : (lt.tm_wday - 1);
  return add_days_local(anchor_s, -since_monday);
}

// Minuit local du 1er du mois contenant `anchor_s`.
int64_t month_start(int64_t anchor_s) {
  time_t t = static_cast<time_t>(anchor_s);
  struct tm lt;
  localtime_r(&t, &lt);
  lt.tm_mday = 1;
  lt.tm_hour = 0;
  lt.tm_min = 0;
  lt.tm_sec = 0;
  lt.tm_isdst = -1;
  return static_cast<int64_t>(mktime(&lt));
}

// Minuit local du 1er du mois SUIVANT `month_start_s` (borne exclusive).
int64_t month_end(int64_t month_start_s) {
  time_t t = static_cast<time_t>(month_start_s);
  struct tm lt;
  localtime_r(&t, &lt);
  lt.tm_mon += 1; // mktime normalise un tm_mon hors [0,11] (déc+1 -> jan N+1)
  lt.tm_isdst = -1;
  return static_cast<int64_t>(mktime(&lt));
}

int64_t shift_month(int64_t anchor_s, int delta_months) {
  time_t t = static_cast<time_t>(month_start(anchor_s));
  struct tm lt;
  localtime_r(&t, &lt);
  lt.tm_mon += delta_months;
  lt.tm_isdst = -1;
  return static_cast<int64_t>(mktime(&lt));
}

// Premier jour de la grille mois : le lundi <= 1er du mois. La grille fait
// toujours 6 semaines pleines, débords des mois voisins compris.
int64_t month_grid_start(int64_t anchor_s) {
  return week_start(month_start(anchor_s));
}

void current_range(int64_t &start_s, int64_t &end_s) {
  if (s_mode == Mode::kWeek) {
    start_s = week_start(s_anchor_s);
    end_s = add_days_local(start_s, 7);
  } else {
    // La fenêtre couvre la GRILLE, pas le mois : les cases de débord affichent
    // les événements de fin du mois précédent et de début du suivant, et un
    // fetch borné au mois les laisserait vides (et cache_covers() les
    // considérerait à tort comme couvertes).
    start_s = month_grid_start(s_anchor_s);
    end_s = add_days_local(start_s, kMonthCells);
  }
}

// Format volontairement compact ("08:30 Titre") : les deux vues affichent
// désormais des colonnes de ~172 px, où un préfixe "Toute la journée — "
// mangerait toute la ligne. Le caractère journée-entière se lit à l'absence
// d'heure.
// `continued` = l'événement a commencé un jour précédent : afficher son heure
// de début ici ferait croire à un rendez-vous du jour. Le chevron U+00BB tient
// dans la plage 0xA0-0xFF des polices montserrat_fr (les flèches U+2190.. n'y
// sont PAS : elles s'afficheraient en carrés vides).
void format_event_line(const gcal::Event &e, bool continued, char *dst,
                       size_t cap) {
  // summary vide = événement importé sans titre : sans repli la ligne se
  // réduirait à une heure orpheline, illisible dans une colonne étroite.
  const char *title = (e.summary[0] != '\0') ? e.summary
                                              : i18n::t(Str::kAgendaUntitled);
  if (continued) {
    snprintf(dst, cap, "» %s", title);
    return;
  }
  if (e.all_day) {
    snprintf(dst, cap, "%s", title);
    return;
  }
  time_t t = static_cast<time_t>(e.start_epoch_s);
  struct tm lt;
  localtime_r(&t, &lt);
  snprintf(dst, cap, "%02d:%02d %s", lt.tm_hour, lt.tm_min, title);
}

// Une ligne par événement du jour, dans `dst` (vide si journée sans
// événement). events_on_day filtre sur la fenêtre du jour : le cache gcal est
// global et peut contenir une autre période (page Météo vs page Agenda).
void format_day_lines(const gcal::EventList &l, int64_t day_start,
                      int64_t day_end, int max_shown, char *dst, size_t cap) {
  dst[0] = '\0';
  const gcal::Event *sel[kWeekMaxPerDay];
  if (max_shown > kWeekMaxPerDay)
    max_shown = kWeekMaxPerDay;
  const int64_t utc_day = utc_day_of(day_start);
  const int n =
      gcal::events_on_day(l, day_start, day_end, utc_day, sel, max_shown);
  for (int i = 0; i < n; ++i) {
    char item[64]; // "»  " (3) + summary (47) + NUL, ou "HH:MM " (6) + summary
    format_event_line(*sel[i], gcal::starts_before_day(*sel[i], day_start,
                                                       utc_day),
                      item, sizeof item);
    if (i > 0)
      strlcat(dst, "\n", cap);
    strlcat(dst, item, cap);
  }
}

void refresh_range_label_locked() {
  char buf[64];
  if (s_mode == Mode::kWeek) {
    int64_t ws = week_start(s_anchor_s);
    time_t t0 = static_cast<time_t>(ws);
    time_t t1 = static_cast<time_t>(add_days_local(ws, 6));
    struct tm l0, l1;
    localtime_r(&t0, &l0);
    localtime_r(&t1, &l1);
    // L'ordre jour/mois appartient à la langue (cf. i18n::month_before_day).
    const bool en = i18n::month_before_day();
    if (l0.tm_mon == l1.tm_mon && en)
      snprintf(buf, sizeof buf, "%s %d - %d, %d", i18n::month_long(l0.tm_mon),
               l0.tm_mday, l1.tm_mday, l0.tm_year + 1900);
    else if (l0.tm_mon == l1.tm_mon)
      snprintf(buf, sizeof buf, "%d - %d %s %d", l0.tm_mday, l1.tm_mday,
               i18n::month_long(l0.tm_mon), l0.tm_year + 1900);
    else if (en)
      snprintf(buf, sizeof buf, "%s %d - %s %d", i18n::month_long(l0.tm_mon),
               l0.tm_mday, i18n::month_long(l1.tm_mon), l1.tm_mday);
    else
      snprintf(buf, sizeof buf, "%d %s - %d %s", l0.tm_mday,
               i18n::month_long(l0.tm_mon), l1.tm_mday,
               i18n::month_long(l1.tm_mon));
  } else {
    time_t t = static_cast<time_t>(month_start(s_anchor_s));
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(buf, sizeof buf, "%s %d", i18n::month_long(lt.tm_mon),
             lt.tm_year + 1900);
  }
  lv_label_set_text(s_range_lbl, buf);
  // Icônes hors des tables de langue : elles ne se traduisent pas, et les
  // tables restent compilables sur host sans LVGL.
  lv_label_set_text_fmt(s_mode_lbl, "%s %s", LV_SYMBOL_LIST,
                        s_mode == Mode::kWeek ? i18n::t(Str::kAgendaMonth)
                                              : i18n::t(Str::kAgendaWeek));
}

void rebuild_week_locked(const gcal::EventList &l) {
  const int64_t ws = week_start(s_anchor_s);
  const int64_t today = today_local_midnight();
  for (int i = 0; i < 7; ++i) {
    const int64_t day_start = add_days_local(ws, i);
    const int64_t day_end = add_days_local(ws, i + 1);
    time_t t = static_cast<time_t>(day_start);
    struct tm lt;
    localtime_r(&t, &lt);
    char day_buf[16];
    // i = 0 est le lundi (week_start) ; tm_wday compte depuis dimanche.
    snprintf(day_buf, sizeof day_buf, "%s %d", i18n::weekday_abbr((i + 1) % 7),
             lt.tm_mday);
    lv_label_set_text(s_week_cols[i].day, day_buf);

    // Repère du jour : dans une grille de 7 colonnes identiques, rien ne dit
    // où on est sans ça.
    const bool is_today = (day_start == today);
    lv_obj_set_style_bg_color(s_week_cols[i].card,
                              lv_color_hex(is_today ? kToday : kCard), 0);
    lv_obj_set_style_border_color(s_week_cols[i].card,
                                  lv_color_hex(is_today ? kWarm : kHair), 0);
    lv_obj_set_style_text_color(s_week_cols[i].day,
                                lv_color_hex(is_today ? kWarm : kSub), 0);

    char lines[512]; // kWeekMaxPerDay (8) x 54 octets + séparateurs
    format_day_lines(l, day_start, day_end, kWeekMaxPerDay, lines,
                     sizeof lines);
    lv_label_set_text(s_week_cols[i].events, lines);
  }
}

void rebuild_month_locked(const gcal::EventList &l) {
  const int64_t ms = month_start(s_anchor_s);
  const int64_t me = month_end(ms);
  const int64_t grid_start = month_grid_start(s_anchor_s);
  const int64_t today = today_local_midnight();
  for (int i = 0; i < kMonthCells; ++i) {
    const int64_t day_s = add_days_local(grid_start, i);
    // Hors du mois affiché = débord de grille (fin du mois précédent, début du
    // suivant). Ces cases restent renseignées : une case vide en tête de grille
    // ne dit pas si le mois commence un mercredi ou si la donnée manque, et la
    // semaine à cheval sur deux mois se lit d'un coup.
    const bool in_month = (day_s >= ms && day_s < me);
    const bool is_today = (day_s == today);
    MonthCell &c = s_month_cells[i];

    lv_obj_set_style_bg_color(
        c.card,
        lv_color_hex(is_today ? kToday : (in_month ? kCard : kOutCard)), 0);
    lv_obj_set_style_border_color(
        c.card, lv_color_hex(is_today ? kWarm : kHair), 0);
    lv_obj_set_style_text_color(
        c.num, lv_color_hex(is_today ? kWarm : (in_month ? kInk : kOutInk)), 0);
    lv_obj_set_style_text_color(
        c.events, lv_color_hex(in_month ? kWarm : kOutInk), 0);

    time_t t = static_cast<time_t>(day_s);
    struct tm lt;
    localtime_r(&t, &lt);
    char buf[4];
    snprintf(buf, sizeof buf, "%d", lt.tm_mday);
    lv_label_set_text(c.num, buf);

    // Les titres remplacent l'ancienne pastille : même information de présence,
    // mais on sait enfin de QUOI il s'agit sans ouvrir la vue semaine.
    char lines[200];
    format_day_lines(l, day_s, add_days_local(day_s, 1), kMonthMaxPerDay, lines,
                     sizeof lines);
    lv_label_set_text(c.events, lines);
  }
}

// Masque la boîte le temps de la reconstruire, même si elle est déjà
// visible (navigation ◀▶, fetch async, timer périodique) : modifier ~42
// cellules (texte + pastille) d'une grille déjà affichée invalide des
// dizaines de petits rectangles épars en une seule passe LVGL — chacun sa
// propre transaction PPA — et sature le pool de transactions du client PPA
// (max_pending_trans_num=2, cf. bsp.cpp) au point où plus aucune bande ne se
// peint (constaté sur cible : "PPA SRM sans fin de transaction" en rafale
// puis "exceed maximum pending transactions", écran figé). LVGL n'invalide
// rien pour un objet masqué : reconstruire à couvert puis ré-afficher ne
// produit qu'UNE invalidation (la boîte entière), débitée en bandes
// régulières comme n'importe quelle autre transition d'écran plein cadre —
// déjà éprouvé ailleurs dans l'appli (ouverture Météo/Agenda, ~1024 kpx sans
// incident).
void refresh_view_locked() {
  const gcal::EventList l = gcal::cached();
  refresh_range_label_locked();
  lv_obj_t *box = (s_mode == Mode::kWeek) ? s_week_box : s_month_box;
  const bool was_visible = !lv_obj_has_flag(box, LV_OBJ_FLAG_HIDDEN);
  if (was_visible)
    lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
  if (s_mode == Mode::kWeek)
    rebuild_week_locked(l);
  else
    rebuild_month_locked(l);
  if (was_visible)
    lv_obj_clear_flag(box, LV_OBJ_FLAG_HIDDEN);
}

// Contexte : tâche gcal_fetch, pas lvgl_task. Aucun appel lv_* ici (cf.
// s_refresh_pending) : juste le drapeau, le rendu proprement dit se fait
// depuis poll_timer_cb au prochain tick LVGL.
void on_fetch_done(bool ok) {
  (void)ok; // en échec le cache reste celui d'avant (inchangé)
  s_refresh_pending.store(true, std::memory_order_relaxed);
}

void poll_timer_cb(lv_timer_t *) {
  if (s_refresh_pending.exchange(false, std::memory_order_relaxed))
    refresh_view_locked();
}

void request_fetch() {
  int64_t start_s, end_s;
  current_range(start_s, end_s);
  gcal::fetch_async(start_s, end_s, on_fetch_done);
}

void cb_prev(lv_event_t *) {
  s_anchor_s = (s_mode == Mode::kWeek) ? add_days_local(s_anchor_s, -7)
                                       : shift_month(s_anchor_s, -1);
  refresh_view_locked();
  request_fetch();
}

void cb_next(lv_event_t *) {
  s_anchor_s = (s_mode == Mode::kWeek) ? add_days_local(s_anchor_s, 7)
                                       : shift_month(s_anchor_s, 1);
  refresh_view_locked();
  request_fetch();
}

void apply_mode_locked() {
  if (s_mode == Mode::kWeek) {
    lv_obj_clear_flag(s_week_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_month_box, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_week_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_month_box, LV_OBJ_FLAG_HIDDEN);
  }
}

void cb_toggle_mode(lv_event_t *) {
  s_mode = (s_mode == Mode::kWeek) ? Mode::kMonth : Mode::kWeek;
  apply_mode_locked();
  refresh_view_locked();
  request_fetch();
}

// Toucher une case de la grille mois ouvre LA SEMAINE de ce jour : trois titres
// par case ne suffisent pas à lire une journée chargée, et c'est le seul geste
// naturel une fois le mois affiché. Les cases de débord y mènent aussi — la
// semaine à cheval sur deux mois est une semaine comme une autre.
// L'index de case est figé à la construction (user_data) ; la date, elle, se
// recalcule au clic depuis l'ancre courante : la grille est repeuplée en place,
// jamais reconstruite objet par objet.
void cb_month_cell(lv_event_t *e) {
  const int idx = static_cast<int>(
      reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  s_anchor_s = add_days_local(month_grid_start(s_anchor_s), idx);
  s_mode = Mode::kWeek;
  apply_mode_locked();
  refresh_view_locked();
  request_fetch();
}

void cb_back(lv_event_t *) {
  if (s_timer)
    lv_timer_pause(s_timer);
  if (s_poll_timer)
    lv_timer_pause(s_poll_timer);
  ui::load_home_screen();
}

void refresh_timer_cb(lv_timer_t *) { request_fetch(); }

// Pause sur SCREEN_UNLOADED et pas seulement au bouton Retour : une sonnerie
// La sonnette ouvre la caméra depuis n'importe quel écran, et les timers de cette
// page continuaient alors de tourner pendant l'appel — jusqu'à lancer un fetch
// HTTP vers HA en pleine session vidéo.
void cb_unloaded(lv_event_t *) {
  if (s_timer)
    lv_timer_pause(s_timer);
  if (s_poll_timer)
    lv_timer_pause(s_poll_timer);
}

lv_obj_t *make_nav_button(lv_obj_t *parent, const char *symbol,
                          lv_event_cb_t cb) {
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_set_size(btn, 64, 64);
  lv_obj_set_style_bg_color(btn, lv_color_hex(kBadge), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(btn, 1, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(kHair), 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, symbol);
  lv_obj_set_style_text_font(lbl, &montserrat_fr_24, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(kInk), 0);
  lv_obj_center(lbl);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
  return btn;
}

} // namespace

void create_screen() {
  s_scr = lv_obj_create(NULL);
  lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s_scr, lv_color_hex(kBg), 0);

  lv_obj_t *title = lv_label_create(s_scr);
  lv_label_set_text_fmt(title, "%s %s", LV_SYMBOL_LIST,
                        i18n::t(Str::kHomeCalendar));
  lv_obj_set_style_text_color(title, lv_color_hex(kInk), 0);
  lv_obj_set_style_text_font(title, &montserrat_fr_36, 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 16);

  // --- Barre de navigation : <  intitulé période  >  [bascule mode] ---
  lv_obj_t *nav = lv_obj_create(s_scr);
  lv_obj_remove_flag(nav, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(nav, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(nav, 0, 0);
  lv_obj_set_size(nav, 480, 72);
  lv_obj_align(nav, LV_ALIGN_TOP_LEFT, 24, 64);
  lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  make_nav_button(nav, LV_SYMBOL_LEFT, cb_prev);
  s_range_lbl = lv_label_create(nav);
  lv_obj_set_style_text_color(s_range_lbl, lv_color_hex(kInk), 0);
  lv_obj_set_style_text_font(s_range_lbl, &montserrat_fr_36, 0);
  make_nav_button(nav, LV_SYMBOL_RIGHT, cb_next);

  // Retour : icône seule, coin haut-droit (comme ui_weather_v3) — libère la
  // bande basse pour la vue semaine/mois au lieu d'un bouton pleine largeur.
  lv_obj_t *back_btn = make_nav_button(s_scr, LV_SYMBOL_LEFT, cb_back);
  lv_obj_align(back_btn, LV_ALIGN_TOP_RIGHT, -24, 12);

  lv_obj_t *mode_btn = lv_button_create(s_scr);
  lv_obj_set_size(mode_btn, 220, 64);
  lv_obj_align(mode_btn, LV_ALIGN_TOP_RIGHT, -24, 84);
  lv_obj_set_style_bg_color(mode_btn, lv_color_hex(kBadge), 0);
  lv_obj_set_style_bg_opa(mode_btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(mode_btn, 1, 0);
  lv_obj_set_style_border_color(mode_btn, lv_color_hex(kHair), 0);
  lv_obj_set_style_shadow_width(mode_btn, 0, 0);
  s_mode_lbl = lv_label_create(mode_btn);
  lv_obj_set_style_text_font(s_mode_lbl, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(s_mode_lbl, lv_color_hex(kInk), 0);
  lv_obj_center(s_mode_lbl);
  lv_obj_add_event_cb(mode_btn, cb_toggle_mode, LV_EVENT_CLICKED, NULL);

  // --- Vue semaine : 7 colonnes jour côte à côte ---
  // Hauteur portée à 630 (contre 598) : la barre de navigation est remontée à
  // 150, et une colonne étroite a besoin de tout le vertical disponible pour
  // empiler ses événements.
  s_week_box = lv_obj_create(s_scr);
  lv_obj_remove_flag(s_week_box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(s_week_box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_week_box, 0, 0);
  lv_obj_set_style_pad_all(s_week_box, 0, 0);
  lv_obj_set_size(s_week_box, LV_PCT(100), kGridH);
  lv_obj_align(s_week_box, LV_ALIGN_TOP_MID, 0, kGridY);
  lv_obj_set_flex_flow(s_week_box, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_hor(s_week_box, 24, 0);
  lv_obj_set_style_pad_column(s_week_box, 4, 0);

  for (int i = 0; i < 7; ++i) {
    // flex_grow : les 7 colonnes se partagent la largeur restante (~172 px
    // chacune sur 1280 moins les marges), sans arithmétique en dur qui
    // casserait au moindre changement de padding.
    lv_obj_t *col = lv_obj_create(s_week_box);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_PCT(100));
    lv_obj_set_style_bg_color(col, lv_color_hex(kCard), 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(col, 8, 0);
    lv_obj_set_style_border_width(col, 1, 0);
    lv_obj_set_style_border_color(col, lv_color_hex(kHair), 0);
    lv_obj_set_style_shadow_width(col, 0, 0);
    ui::theme::card_frame(col);
    lv_obj_set_style_pad_all(col, 8, 0);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    s_week_cols[i].card = col;

    s_week_cols[i].day = lv_label_create(col);
    lv_obj_set_style_text_color(s_week_cols[i].day, lv_color_hex(kSub), 0);
    lv_obj_set_style_text_font(s_week_cols[i].day, &montserrat_fr_24, 0);

    // Police 16 : à 24 px une colonne de 172 px ne tient que ~7 caractères,
    // soit une heure et rien d'autre.
    s_week_cols[i].events = lv_label_create(col);
    lv_obj_set_width(s_week_cols[i].events, LV_PCT(100));
    lv_obj_set_flex_grow(s_week_cols[i].events, 1);
    lv_label_set_long_mode(s_week_cols[i].events, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_week_cols[i].events, lv_color_hex(kInk), 0);
    lv_obj_set_style_text_font(s_week_cols[i].events, &montserrat_fr_16, 0);
    lv_obj_set_style_text_line_space(s_week_cols[i].events, 4, 0);
  }

  // --- Vue mois : grille 7x6, masquée par défaut ---
  // Cellules agrandies (96 -> ~101 px de haut) en récupérant la marge basse
  // inutilisée : il faut la place d'empiler le numéro du jour ET les titres.
  s_month_box = lv_obj_create(s_scr);
  lv_obj_remove_flag(s_month_box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(s_month_box, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_month_box, 0, 0);
  lv_obj_set_style_pad_all(s_month_box, 0, 0);
  lv_obj_set_size(s_month_box, LV_PCT(100), kGridH);
  lv_obj_align(s_month_box, LV_ALIGN_TOP_MID, 0, kGridY);
  lv_obj_set_style_pad_hor(s_month_box, 24, 0);
  lv_obj_set_flex_flow(s_month_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_month_box, 4, 0);
  lv_obj_add_flag(s_month_box, LV_OBJ_FLAG_HIDDEN);

  for (int w = 0; w < 6; ++w) {
    lv_obj_t *row = lv_obj_create(s_month_box);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    // flex_grow vertical : les 6 semaines se partagent kGridH, sans hauteur en
    // dur à réajuster à chaque retouche de la grille.
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 4, 0);
    for (int d = 0; d < 7; ++d) {
      int idx = w * 7 + d;
      lv_obj_t *cell = lv_obj_create(row);
      lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_flex_grow(cell, 1);
      lv_obj_set_height(cell, LV_PCT(100));
      lv_obj_set_style_bg_color(cell, lv_color_hex(kCard), 0);
      lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(cell, 6, 0);
      lv_obj_set_style_border_width(cell, 1, 0);
      lv_obj_set_style_border_color(cell, lv_color_hex(kHair), 0);
      lv_obj_set_style_shadow_width(cell, 0, 0);
      ui::theme::card_frame(cell);
      lv_obj_set_style_pad_all(cell, 5, 0);
      lv_obj_set_style_pad_row(cell, 2, 0);
      lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                            LV_FLEX_ALIGN_START);
      // lv_obj est déjà LV_OBJ_FLAG_CLICKABLE par défaut ; les labels enfants
      // ne le sont pas, le tap leur traverse dessus jusqu'à la cellule.
      lv_obj_add_event_cb(cell, cb_month_cell, LV_EVENT_CLICKED,
                          reinterpret_cast<void *>(
                              static_cast<intptr_t>(idx)));
      s_month_cells[idx].card = cell;

      s_month_cells[idx].num = lv_label_create(cell);
      lv_obj_set_style_text_color(s_month_cells[idx].num, lv_color_hex(kInk),
                                  0);
      lv_obj_set_style_text_font(s_month_cells[idx].num,
                                 &montserrat_fr_24, 0);

      // Remplace la pastille : même signal de présence, mais avec le titre.
      s_month_cells[idx].events = lv_label_create(cell);
      lv_obj_set_width(s_month_cells[idx].events, LV_PCT(100));
      lv_obj_set_flex_grow(s_month_cells[idx].events, 1);
      lv_label_set_long_mode(s_month_cells[idx].events, LV_LABEL_LONG_DOT);
      lv_obj_set_style_text_color(s_month_cells[idx].events,
                                  lv_color_hex(kWarm), 0);
      lv_obj_set_style_text_font(s_month_cells[idx].events, &montserrat_fr_16,
                                 0);
      lv_obj_set_style_text_line_space(s_month_cells[idx].events, 2, 0);
    }
  }

  s_anchor_s = today_local_midnight();
  refresh_range_label_locked();

  // Timers créés en pause : activés à l'ouverture (cf. pattern ui_mic).
  s_timer = lv_timer_create(refresh_timer_cb, kRefreshPeriodMs, NULL);
  lv_timer_pause(s_timer);
  s_poll_timer = lv_timer_create(poll_timer_cb, kPollPeriodMs, NULL);
  lv_timer_pause(s_poll_timer);
  lv_obj_add_event_cb(s_scr, cb_unloaded, LV_EVENT_SCREEN_UNLOADED, NULL);
}

void open_screen_from_event() {
  s_mode = Mode::kWeek;
  apply_mode_locked();
  s_anchor_s = today_local_midnight();
  refresh_view_locked();
  request_fetch();
  if (s_timer)
    lv_timer_resume(s_timer);
  if (s_poll_timer)
    lv_timer_resume(s_poll_timer);
  /* Sans animation : cf. ui::load_home_screen(). Le peuplement de la grille
   * reste AVANT le chargement et non déféré à LV_EVENT_SCREEN_LOADED — sans
   * anim, tout se déroule dans la même passe de lv_timer_handler et ne produit
   * qu'UN repaint. Le différer en ferait deux (grille vide, puis remplie). */
  lv_scr_load(s_scr);
}

bool is_active() { return s_scr != nullptr && lv_screen_active() == s_scr; }

} // namespace ui_agenda
