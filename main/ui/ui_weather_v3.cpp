#include "ui_weather_v3.h"

#include "bsp.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui.h"
#include "ui_call_viewer.h"
#include "ui_stats.h"
#include "lottie_frames.h"
#include "app/call_history.h"
#include "app/ephemeris.h"
#include "net/gcal.h"
#include "net/weather.h"
#include "net/wifi_net.h"
#include "net/esphome_api.h"
#include "theme.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

// Écran météo "V3" : carte du jour (haut) + bande de 6 prévisions (bas), thème
// CLAIR sobre et professionnel. Réutilise 100% de la couche données
// (weather::cached / fetch_async) ET l'infrastructure Lottie éprouvée de
// ui_weather.cpp (slots + chargement échelonné + pause hors-écran) —
// indispensable pour tenir le budget CPU/WDT avec 7 animations ThorVG.
// Depuis 2026-11-15, les icônes sont des lv_image servies par le cache
// lottie_frames (une boucle rendue une fois) : 75-98 % du cœur 0 -> 13 %.
//
// Contrainte matérielle (test device 19/07) : ombres portées + halos DÉGRADÉS
// derrière les icônes animées => recomposition par frame => CPU coeur A à 97%.
// Ici : cartes OPAQUES à filet 1px (pas d'ombre), halos = disques pleins
// OPAQUES (remplissage plat, pas de dégradé ni de blur) => coût de rendu plat
// même redessiné à chaque frame sous l'icône. Voir aussi la note dans le
// commit de correction CPU.

LV_FONT_DECLARE(montserrat_fr_24);
LV_FONT_DECLARE(montserrat_fr_36);
LV_FONT_DECLARE(montserrat_fr_64);
LV_FONT_DECLARE(montserrat_fr_96);
LV_FONT_DECLARE(montserrat_fr_112);
LV_FONT_DECLARE(montserrat_fr_144);

// Icônes météo animées — mêmes blobs Lottie embarqués que ui_weather.cpp
// (EMBED_FILES). Décls dupliquées ici (simples symboles de l'éditeur de
// liens, pas de stockage) pour garder cet écran autonome sans toucher au
// code existant.
extern "C" const uint8_t
    clear_day_json_start[] asm("_binary_clear_day_json_start");
extern "C" const uint8_t clear_day_json_end[] asm("_binary_clear_day_json_end");
extern "C" const uint8_t
    clear_night_json_start[] asm("_binary_clear_night_json_start");
extern "C" const uint8_t
    clear_night_json_end[] asm("_binary_clear_night_json_end");
extern "C" const uint8_t
    partly_cloudy_day_json_start[] asm("_binary_partly_cloudy_day_json_start");
extern "C" const uint8_t
    partly_cloudy_day_json_end[] asm("_binary_partly_cloudy_day_json_end");
extern "C" const uint8_t cloudy_json_start[] asm("_binary_cloudy_json_start");
extern "C" const uint8_t cloudy_json_end[] asm("_binary_cloudy_json_end");
extern "C" const uint8_t fog_json_start[] asm("_binary_fog_json_start");
extern "C" const uint8_t fog_json_end[] asm("_binary_fog_json_end");
extern "C" const uint8_t rain_json_start[] asm("_binary_rain_json_start");
extern "C" const uint8_t rain_json_end[] asm("_binary_rain_json_end");
extern "C" const uint8_t
    extreme_rain_json_start[] asm("_binary_extreme_rain_json_start");
extern "C" const uint8_t
    extreme_rain_json_end[] asm("_binary_extreme_rain_json_end");
extern "C" const uint8_t snow_json_start[] asm("_binary_snow_json_start");
extern "C" const uint8_t snow_json_end[] asm("_binary_snow_json_end");
extern "C" const uint8_t sleet_json_start[] asm("_binary_sleet_json_start");
extern "C" const uint8_t sleet_json_end[] asm("_binary_sleet_json_end");
extern "C" const uint8_t hail_json_start[] asm("_binary_hail_json_start");
extern "C" const uint8_t hail_json_end[] asm("_binary_hail_json_end");
extern "C" const uint8_t
    thunderstorms_json_start[] asm("_binary_thunderstorms_json_start");
extern "C" const uint8_t
    thunderstorms_json_end[] asm("_binary_thunderstorms_json_end");
extern "C" const uint8_t thunderstorms_rain_json_start[] asm(
    "_binary_thunderstorms_rain_json_start");
extern "C" const uint8_t
    thunderstorms_rain_json_end[] asm("_binary_thunderstorms_rain_json_end");
extern "C" const uint8_t wind_json_start[] asm("_binary_wind_json_start");
extern "C" const uint8_t wind_json_end[] asm("_binary_wind_json_end");
extern "C" const uint8_t tornado_json_start[] asm("_binary_tornado_json_start");
extern "C" const uint8_t tornado_json_end[] asm("_binary_tornado_json_end");

namespace ui_weather_v3 {

static const char *TAG = "ui_wx_v3";

static constexpr int64_t kCacheMaxAgeUs = 30LL * 60 * 1000 * 1000;
static constexpr uint32_t kRefreshPeriodMs = 30 * 60 * 1000;
static constexpr uint32_t kIconStaggerMs = 80;
static constexpr uint32_t kClockPeriodMs = 15 * 1000; // rafraîchit date/heure
// 2 s : kClockPeriodMs est trop lent pour un état de lien, on veut voir la
// coupure quasi tout de suite.
static constexpr uint32_t kNetPeriodMs = 2 * 1000;
static constexpr int kIconCount = 7;                  // hero + 6 jours
// Lignes d'événements du jour pré-créées : de quoi remplir toute la hauteur de
// la colonne agenda en montserrat_fr_24 (~300 px / 29). Au-delà, la dernière
// ligne devient « +N autres » — c'est la hauteur qui borne, pas un plafond.
static constexpr int kEventLines = 12;
// Horizon de recherche du prochain événement. Le fetch de la page couvre donc
// 30 jours et non plus la seule journée. gcal::EventList plafonne à 64
// événements dans l'ordre renvoyé par HA (chronologique) : ceux du jour, en
// tête, ne sont jamais ceux qu'on perd.
static constexpr int64_t kNextHorizonS = 30LL * 24 * 3600;
static constexpr int32_t kEventsPad = 8;  // pad vertical des pastilles agenda
static constexpr int32_t kEventsGap = 10; // écart entre les deux pastilles

// Noms FR (SNTP fournit tm_wday 0=dimanche, tm_mon 0=janvier).
static const char *kWeekFr[7] = {"Dimanche", "Lundi",    "Mardi", "Mercredi",
                                 "Jeudi",    "Vendredi", "Samedi"};
static const char *kMonthFr[12] = {
    "janvier", "février", "mars",      "avril",   "mai",      "juin",
    "juillet", "août",    "septembre", "octobre", "novembre", "décembre"};
static const char *kWeekShortFr[7] = {"dim.", "lun.", "mar.", "mer.",
                                      "jeu.", "ven.", "sam."};
static const char *kMonthShortFr[12] = {"janv.", "févr.", "mars", "avr.",
                                        "mai",   "juin",  "juil.", "août",
                                        "sept.", "oct.",  "nov.", "déc."};

// --- Palette thème clair ----------------------------------------------------
static const uint32_t &kBg = ui::theme::active.bg; // fond général gris-bleu très clair
static const uint32_t &kCard = ui::theme::active.card; // cartes blanc cassé
static const uint32_t &kInk = ui::theme::active.ink;  // texte principal (anthracite)
static const uint32_t &kSub = ui::theme::active.sub;  // texte secondaire (gris moyen)
static const uint32_t &kHair = ui::theme::active.hair; // filet / séparateur discret
static const uint32_t &kWarm = ui::theme::active.warm; // accent chaud (température max)
static const uint32_t &kCold = ui::theme::active.cold; // accent froid (température min)
static const uint32_t &kNextBg = ui::theme::active.next_bg;  // pastille prochain événement
static const uint32_t &kNextInk = ui::theme::active.next_ink; // texte sur kNextBg
static const uint32_t &kBadge = ui::theme::active.badge;    // fond bouton retour clair
static const uint32_t &kNetOk = ui::theme::active.ok;    // lien établi (vert)
static const uint32_t &kNetKo = ui::theme::active.ko;    // lien absent (rouge)

static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_status = nullptr;
static lv_timer_t *s_refresh_timer = nullptr;
static lv_timer_t *s_icon_stagger_timer = nullptr;
static lv_timer_t *s_clock_timer = nullptr;
static lv_obj_t *s_net_wifi = nullptr;
static lv_obj_t *s_net_ha = nullptr;
static lv_timer_t *s_net_timer = nullptr;
static bool s_screen_active = false;
// Animations Lottie en cours (et non en pause). Distinct de s_screen_active :
// écran affiché mais rétroéclairage coupé (extinction HA, veille de nuit), les
// icônes doivent s'arrêter — la Météo est l'écran de repos et rendait ses 7
// Lottie à 30 fps toute la nuit dans le noir.
static bool s_icons_running = false;
// Cadence des icônes (lottie_frames::kFps). Un seul timer pour les 7 : les
// icônes d'une même entrée du cache partagent ainsi leur image du top.
static lv_timer_t *s_anim_timer = nullptr;
static uint32_t s_anim_tick = 0;
// Notification d'appels manqués : flottante (IGNORE_LAYOUT) et créée en
// dernier, donc au-dessus de tout le reste de l'écran.
static lv_obj_t *s_missed_box = nullptr;
static lv_obj_t *s_missed_lbl = nullptr;

// Icône = simple lv_image pointée sur une image du cache lottie_frames : plus
// de lv_lottie (7 rendus ThorVG à 30 img/s = 75-98 % du cœur 0, mesuré le
// 2026-11-15). Changer d'icône = changer d'entrée de cache, le widget reste.
struct IconSlot {
  lv_obj_t *widget = nullptr;
  int32_t px = 0;
  const uint8_t *cur = nullptr;
  lottie_frames::Anim *anim = nullptr;
  const lv_image_dsc_t *shown = nullptr;
};
static IconSlot *s_all_icons[kIconCount] = {};

struct PendingIcon {
  IconSlot *slot;
  const uint8_t *data;
  size_t size;
};
static PendingIcon s_pending_icons[kIconCount];
static int s_pending_count = 0;
static int s_pending_pos = 0;

// Une carte de prévision (bande basse) : liseré coloré + jour + icône +
// max/min.
struct DayCol {
  lv_obj_t *liser; // filet coloré fin en haut
  lv_obj_t *day;
  IconSlot icon;
  lv_obj_t *tmax;
  lv_obj_t *tmin;
};
static DayCol s_days[6];

struct Hero {
  lv_obj_t *date;  // date du jour en toutes lettres (haut-gauche)
  lv_obj_t *saint; // éphéméride, à droite de la date (gris, plus petit)
  lv_obj_t *clock; // heure HH:MM (badge, haut-droite)
  IconSlot icon;
  lv_obj_t *tmax; // max du jour (orange, XXL)
  lv_obj_t *tmin; // min du jour (bleu)
  lv_obj_t *cond; // libellé condition
  lv_obj_t *meta; // "Humidité 60%   ·   Vent 12 km/h"
  lv_obj_t *events_col;  // colonne agenda, pleine hauteur à droite
  lv_obj_t *today_badge; // pastille kWarm — événements du jour
  lv_obj_t *today_tag;
  lv_obj_t *today_lines[kEventLines];
  lv_obj_t *next_badge; // pastille orange clair — prochain événement futur
  lv_obj_t *next_tag;   // "PROCHAIN · mar. 27 oct."
  lv_obj_t *next_line;
};
static Hero s_hero;

struct CondIcon {
  const char *cond;
  const uint8_t *data;
  size_t size;
};
static const CondIcon kCondIcons[] = {
    {"sunny", clear_day_json_start,
     static_cast<size_t>(clear_day_json_end - clear_day_json_start)},
    {"clear-night", clear_night_json_start,
     static_cast<size_t>(clear_night_json_end - clear_night_json_start)},
    {"partlycloudy", partly_cloudy_day_json_start,
     static_cast<size_t>(partly_cloudy_day_json_end -
                         partly_cloudy_day_json_start)},
    {"cloudy", cloudy_json_start,
     static_cast<size_t>(cloudy_json_end - cloudy_json_start)},
    {"fog", fog_json_start, static_cast<size_t>(fog_json_end - fog_json_start)},
    {"rainy", rain_json_start,
     static_cast<size_t>(rain_json_end - rain_json_start)},
    {"pouring", extreme_rain_json_start,
     static_cast<size_t>(extreme_rain_json_end - extreme_rain_json_start)},
    {"snowy", snow_json_start,
     static_cast<size_t>(snow_json_end - snow_json_start)},
    {"snowy-rainy", sleet_json_start,
     static_cast<size_t>(sleet_json_end - sleet_json_start)},
    {"hail", hail_json_start,
     static_cast<size_t>(hail_json_end - hail_json_start)},
    {"lightning", thunderstorms_json_start,
     static_cast<size_t>(thunderstorms_json_end - thunderstorms_json_start)},
    {"lightning-rainy", thunderstorms_rain_json_start,
     static_cast<size_t>(thunderstorms_rain_json_end -
                         thunderstorms_rain_json_start)},
    {"windy", wind_json_start,
     static_cast<size_t>(wind_json_end - wind_json_start)},
    {"windy-variant", wind_json_start,
     static_cast<size_t>(wind_json_end - wind_json_start)},
    {"exceptional", tornado_json_start,
     static_cast<size_t>(tornado_json_end - tornado_json_start)},
};

static const CondIcon &cond_icon(const char *c) {
  for (const CondIcon &ic : kCondIcons)
    if (strcmp(ic.cond, c) == 0)
      return ic;
  static const CondIcon kFallback = {
      "", cloudy_json_start,
      static_cast<size_t>(cloudy_json_end - cloudy_json_start)};
  return kFallback;
}

static const char *cond_label_fr(const char *c) {
  struct L {
    const char *k;
    const char *fr;
  };
  static const L kL[] = {
      {"sunny", "Ensoleillé"},
      {"clear-night", "Ciel dégagé"},
      {"partlycloudy", "Éclaircies"},
      {"cloudy", "Nuageux"},
      {"fog", "Brouillard"},
      {"rainy", "Pluie"},
      {"pouring", "Fortes pluies"},
      {"snowy", "Neige"},
      {"snowy-rainy", "Pluie et neige"},
      {"hail", "Grêle"},
      {"lightning", "Orages"},
      {"lightning-rainy", "Orages pluvieux"},
      {"windy", "Venteux"},
      {"windy-variant", "Venteux"},
      {"exceptional", "Conditions extrêmes"},
  };
  for (const L &l : kL)
    if (strcmp(l.k, c) == 0)
      return l.fr;
  return "";
}

// Paliers de température — sert au liseré coloré fin des cartes de prévision.
static lv_color_t temp_color(float v) {
  if (std::isnan(v))
    return lv_color_hex(kHair);
  if (v < 5)
    return lv_color_hex(0x2F6FEB);
  if (v < 15)
    return lv_color_hex(0x0F9BB0);
  if (v < 23)
    return lv_color_hex(0x1FA85A);
  if (v < 28)
    return lv_color_hex(0xE0930A);
  return lv_color_hex(0xE0451E);
}

// Lit l'horloge système (SNTP, cf. wifi_net) et met à jour date + heure.
// Affiche "--" tant que la synchro n'a pas eu lieu (année < 2024 = epoch 1970).
// Mutex LVGL requis.
static void update_clock_locked() {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  if (t.tm_year < 124) {
    lv_label_set_text(s_hero.date, "-- --- ----");
    lv_label_set_text(s_hero.saint, "");
    lv_label_set_text(s_hero.clock, "--:--");
    return;
  }
  // Réécrit seulement ce qui change : lv_label_set_text ne compare pas et
  // invalide toujours, et la date relance le layout de toute la colonne flex.
  static int s_shown_yday = -1;
  if (t.tm_yday != s_shown_yday) {
    s_shown_yday = t.tm_yday;
    char d[48];
    snprintf(d, sizeof d, "%s %d %s", kWeekFr[t.tm_wday], t.tm_mday,
             kMonthFr[t.tm_mon]);
    lv_label_set_text(s_hero.date, d);
    lv_label_set_text(s_hero.saint,
                      ephemeris::saint_of_day(t.tm_mon, t.tm_mday));
  }
  char hm[8];
  snprintf(hm, sizeof hm, "%02d:%02d", t.tm_hour, t.tm_min);
  if (strcmp(lv_label_get_text(s_hero.clock), hm) != 0) {
    lv_label_set_text(s_hero.clock, hm);
  }
}

// Recalé à chaque tick sur la minute suivante (+50 ms) : à période fixe de
// 15 s, l'heure affichée pouvait retarder de 15 s sur l'heure réelle.
static void clock_timer_cb(lv_timer_t *timer) {
  update_clock_locked();
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  uint32_t next = (uint32_t)(60 - t.tm_sec) * 1000 + 50;
  if (t.tm_year < 124 || next > 61000) {
    next = kClockPeriodMs; // horloge pas encore synchronisée
  }
  lv_timer_set_period(timer, next);
}

// Fenêtre "aujourd'hui" en heure locale (minuit -> minuit+1j), convertie en
// epoch pour le fetch gcal. Le fuseau est réglé par wifi_net::connect_blocking
// (setenv("TZ", ...)) avant que cette page soit atteignable.
static void today_range_local(int64_t &start_s, int64_t &end_s) {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  t.tm_hour = 0;
  t.tm_min = 0;
  t.tm_sec = 0;
  t.tm_isdst = -1;
  start_s = static_cast<int64_t>(mktime(&t));
  end_s = start_s + 24 * 3600;
}

// Fenêtre de fetch agenda de la page : aujourd'hui + l'horizon du prochain
// événement.
static void agenda_range_local(int64_t &start_s, int64_t &end_s) {
  int64_t day_end;
  today_range_local(start_s, day_end);
  end_s = start_s + kNextHorizonS;
}

// "HH:MM — titre", "Toute la journée — titre" ou "En cours — titre".
static void format_event_item(const gcal::Event &e, int64_t day_start,
                              int64_t utc_day, char *dst, size_t n) {
  if (gcal::starts_before_day(e, day_start, utc_day)) {
    // Événement de plusieurs jours entamé avant aujourd'hui : son heure de
    // début n'a pas lieu ce jour-ci.
    snprintf(dst, n, "En cours — %s", e.summary);
  } else if (e.all_day) {
    snprintf(dst, n, "Toute la journée — %s", e.summary);
  } else {
    time_t t = static_cast<time_t>(e.start_epoch_s);
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(dst, n, "%02d:%02d — %s", lt.tm_hour, lt.tm_min, e.summary);
  }
}

// Premier événement qui COMMENCE après aujourd'hui (le cache est trié). Les
// journées entières sont ancrées à minuit UTC (cf. gcal::events_on_day), d'où
// la seconde borne.
static const gcal::Event *find_next_event(const gcal::EventList &l,
                                          int64_t day_end, int64_t utc_day) {
  for (int i = 0; i < l.count; ++i) {
    const gcal::Event &e = l.events[i];
    if (e.all_day ? e.start_epoch_s >= utc_day + 24 * 3600
                  : e.start_epoch_s >= day_end)
      return &e;
  }
  return nullptr;
}

// Rejoue le cache gcal dans la colonne agenda du hero. Mutex LVGL requis.
//
// Le filtrage sur la fenêtre du jour est OBLIGATOIRE ici : le cache gcal est
// global et la page Agenda le remplit avec la semaine ou le mois qu'elle
// affiche. Cette fonction se fiait autrefois aux bornes du dernier fetch — il
// suffisait de naviguer d'une semaine dans l'Agenda puis de revenir sur Météo
// pour voir les événements d'une autre semaine listés sous "Aujourd'hui".
static void update_events_locked() {
  const gcal::EventList l = gcal::cached();
  int64_t day_start, day_end;
  today_range_local(day_start, day_end);
  const gcal::Event *today[gcal::EventList::kMax] = {};
  // Ancre journée-entière : minuit UTC de la même date civile (les événements
  // sans heure sont datés en UTC par le parseur, la fenêtre ci-dessus est
  // locale). Cf. gcal::events_on_day.
  struct tm dt;
  time_t day_t = static_cast<time_t>(day_start);
  localtime_r(&day_t, &dt);
  const int64_t utc_day =
      gcal::utc_midnight(dt.tm_year + 1900, dt.tm_mon + 1, dt.tm_mday);
  const int shown = gcal::events_on_day(l, day_start, day_end, utc_day, today,
                                        gcal::EventList::kMax);

  // 80 et non 64 : "Toute la journée — " pèse 22 octets en UTF-8 et summary
  // en fait 47 au pire — la troncature est une erreur (-Werror).
  char item[80];

  // --- Prochain événement (pastille orange clair) ---
  // Seulement si le cache couvre l'horizon : rempli par l'Agenda sur une autre
  // période, il désignerait un « prochain » qui n'en est pas un.
  const gcal::Event *next =
      gcal::cache_covers(day_start, day_start + kNextHorizonS, kCacheMaxAgeUs)
          ? find_next_event(l, day_end, utc_day)
          : nullptr;
  if (next != nullptr) {
    // Journée entière : la date est celle de l'ancre UTC, pas de l'heure locale
    // (minuit UTC = la veille au soir à l'ouest de Greenwich).
    time_t t = static_cast<time_t>(next->start_epoch_s);
    struct tm nt;
    if (next->all_day)
      gmtime_r(&t, &nt);
    else
      localtime_r(&t, &nt);
    char tag[48];
    snprintf(tag, sizeof tag, "PROCHAIN · %s %d %s", kWeekShortFr[nt.tm_wday],
             nt.tm_mday, kMonthShortFr[nt.tm_mon]);
    lv_label_set_text(s_hero.next_tag, tag);
    if (next->all_day)
      snprintf(item, sizeof item, "Toute la journée — %s", next->summary);
    else
      snprintf(item, sizeof item, "%02d:%02d — %s", nt.tm_hour, nt.tm_min,
               next->summary);
    lv_label_set_text(s_hero.next_line, item);
    lv_obj_clear_flag(s_hero.next_badge, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_hero.next_badge, LV_OBJ_FLAG_HIDDEN);
  }

  // --- Événements du jour : autant que la hauteur le permet ---
  for (lv_obj_t *line : s_hero.today_lines)
    lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
  if (shown == 0) {
    // Pas de pastille orange vide les jours sans événement.
    lv_obj_add_flag(s_hero.today_badge, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_clear_flag(s_hero.today_badge, LV_OBJ_FLAG_HIDDEN);

  // Hauteur disponible mesurée et non calculée : elle découle de la carte du
  // jour (flex), et une constante divergerait à la prochaine retouche.
  lv_obj_update_layout(s_hero.events_col);
  int32_t avail = lv_obj_get_content_height(s_hero.events_col) -
                  2 * kEventsPad - lv_font_get_line_height(&montserrat_fr_24);
  if (next != nullptr)
    avail -= lv_obj_get_height(s_hero.next_badge) + kEventsGap;

  // Police dégressive : 36 tant que tout tient, puis 24, puis troncature.
  const lv_font_t *font = &montserrat_fr_36;
  int32_t lh = lv_font_get_line_height(font);
  if (shown * lh > avail) {
    font = &montserrat_fr_24;
    lh = lv_font_get_line_height(font);
  }
  int cap = avail / lh;
  if (cap < 1)
    cap = 1;
  if (cap > kEventLines)
    cap = kEventLines;
  const bool overflow = shown > cap;
  const int listed = overflow ? cap - 1 : shown;

  for (int i = 0; i <= listed && i < kEventLines; ++i) {
    if (i == listed && !overflow)
      break;
    if (i < listed)
      format_event_item(*today[i], day_start, utc_day, item, sizeof item);
    else
      snprintf(item, sizeof item, "+%d autres", shown - listed);
    lv_obj_t *line = s_hero.today_lines[i];
    lv_label_set_text(line, item);
    lv_obj_set_style_text_font(line, font, 0);
    // Hauteur = une ligne : c'est elle qui fait couper LONG_DOT au lieu de
    // passer à la ligne (un titre long décalerait tout le décompte).
    lv_obj_set_height(line, lh);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_HIDDEN);
  }
}

// Contexte : tâche gcal_fetch (PAS la tâche LVGL) -> LvglLock obligatoire.
static void on_gcal_fetch_done(bool ok) {
  (void)ok; // en échec le cache reste celui d'avant (inchangé)
  bsp::Bsp::LvglLock lvgl_lock;
  // Écran quitté entre-temps (caméra ouverte par une sonnerie) : le cache est
  // à jour, SCREEN_LOADED relira. Re-layouter un écran caché pendant un appel
  // ne servait à rien.
  if (!s_screen_active) {
    return;
  }
  update_events_locked();
}

static void refresh_timer_cb(lv_timer_t *);

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
    // Home Assistant (re)connected: forecast and agenda come through it. A
    // board that booted before HA answered would otherwise show "--" until
    // the 30-min refresh.
    if (ha && s_ha_shown == 0) {
      refresh_timer_cb(nullptr);
    }
    s_ha_shown = ha;
    lv_obj_set_style_text_color(s_net_ha,
                                lv_color_hex(ha ? kNetOk : kNetKo), 0);
  }
}

// Montre l'image du top courant. Mutex LVGL requis.
static void show_icon_frame_locked(IconSlot &s) {
  if (!s.anim)
    return;
  bool in_place = false;
  const lv_image_dsc_t *d =
      lottie_frames::frame(s.anim, s_anim_tick, &in_place);
  if (d != s.shown) {
    lv_image_set_src(s.widget, d);
    s.shown = d;
  } else if (in_place) {
    lv_obj_invalidate(s.widget);
  }
}

static void anim_timer_cb(lv_timer_t *) {
  s_anim_tick++;
  for (IconSlot *slot : s_all_icons) {
    if (slot)
      show_icon_frame_locked(*slot);
  }
}

// Pause/reprise des icônes selon le rétroéclairage : écran éteint = aucun
// rendu, ni ThorVG (remplissage du cache) ni LVGL.
static void set_icons_running_locked(bool run) {
  if (run == s_icons_running) {
    return;
  }
  s_icons_running = run;
  if (!s_anim_timer)
    return;
  if (run)
    lv_timer_resume(s_anim_timer);
  else
    lv_timer_pause(s_anim_timer);
}

// Rend toutes les entrées du cache : l'écran quitté, ses ~10 Mo de PSRAM
// reviennent à la session vidéo qui l'a peut-être chassé. Mutex LVGL requis.
static void release_icons_locked() {
  for (IconSlot *slot : s_all_icons) {
    if (!slot)
      continue;
    lottie_frames::release(slot->anim);
    slot->anim = nullptr;
    slot->cur = nullptr;
    slot->shown = nullptr;
    if (slot->widget)
      lv_image_set_src(slot->widget, nullptr);
  }
}

static void net_timer_cb(lv_timer_t *) {
  update_net_locked();
  set_icons_running_locked(bsp::Bsp::instance().display_on());
}

// --- Notification d'appels manqués ------------------------------------------

// Rejouée à CHAQUE chargement de l'écran, et pas seulement au retour d'un appel
// manqué : le compteur ne retombe à zéro qu'une fois la capture réellement
// ouverte, la notification doit donc réapparaître tant que ce n'est pas fait.
// Mutex LVGL requis.
static void update_missed_locked() {
  if (!s_missed_box) {
    return;
  }
  const uint32_t n = call_history::missed_count();
  if (n == 0) {
    lv_obj_add_flag(s_missed_box, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  char buf[48];
  snprintf(buf, sizeof buf, "%lu appel%s manqué%s", (unsigned long)n,
           n > 1 ? "s" : "", n > 1 ? "s" : "");
  lv_label_set_text(s_missed_lbl, buf);
  lv_obj_remove_flag(s_missed_box, LV_OBJ_FLAG_HIDDEN);
}

static void cb_missed_view(lv_event_t *) {
  // Masquée AVANT la navigation : sinon elle réapparaîtrait telle quelle au
  // retour de la visionneuse, le temps que le compteur acquitté soit relu.
  lv_obj_add_flag(s_missed_box, LV_OBJ_FLAG_HIDDEN);
  ui_call_viewer::open_screen_from_event(call_history::last_shot_path(),
                                         open_screen);
}

// Fermer la notification ne remet PAS le compteur à zéro : seule l'ouverture
// effective de la capture prouve qu'on a vu qui a sonné.
static void cb_missed_close(lv_event_t *) {
  lv_obj_add_flag(s_missed_box, LV_OBJ_FLAG_HIDDEN);
}

static void queue_icon_locked(IconSlot &slot, const CondIcon &ic) {
  // Écran pas affiché (création au boot) : rien à rendre ni à réserver,
  // SCREEN_LOADED rejoue refresh_locked.
  if (!s_screen_active)
    return;
  if (slot.cur == ic.data)
    return;
  if (s_pending_count >= kIconCount)
    return;
  s_pending_icons[s_pending_count++] = {&slot, ic.data, ic.size};
}

// Acquiert la nouvelle entrée AVANT de rendre l'ancienne : deux jours qui
// échangent la même icône gardent leur cache. Le parse ThorVG reste étalé
// par icon_stagger_cb (budget WDT).
static void load_icon_locked(IconSlot &s, const uint8_t *data, size_t size) {
  if (!s.widget)
    return;
  lottie_frames::Anim *next = lottie_frames::acquire(data, size, s.px, kCard);
  lottie_frames::release(s.anim);
  s.anim = next;
  s.cur = data;
  s.shown = nullptr;
  if (s.anim) {
    show_icon_frame_locked(s);
  } else {
    lv_image_set_src(s.widget, nullptr);
  }
}

static void icon_stagger_cb(lv_timer_t *timer) {
  if (s_pending_pos >= s_pending_count) {
    lv_timer_delete(timer);
    s_icon_stagger_timer = nullptr;
    return;
  }
  const PendingIcon &p = s_pending_icons[s_pending_pos++];
  load_icon_locked(*p.slot, p.data, p.size);
}

static void start_icon_stagger_locked() {
  s_pending_pos = 0;
  if (s_pending_count == 0)
    return;
  if (!s_icon_stagger_timer)
    s_icon_stagger_timer =
        lv_timer_create(icon_stagger_cb, kIconStaggerMs, nullptr);
}

// Met à jour hero + bande hebdo depuis weather::cached(). Mutex LVGL requis.
static void refresh_locked() {
  s_pending_count = 0;
  s_pending_pos = 0;

  // Copie et non référence : le cache est réécrit par la tâche weather_fetch.
  // Une référence exposait une lecture déchirée en plein milieu de la mise à
  // jour (cf. weather::cached).
  const weather::Forecast f = weather::cached();
  const weather::CurrentConditions &c = f.current;
  const char *cond = c.valid ? c.condition : "";

  // --- Carte du jour (hero) -------------------------------------------------
  queue_icon_locked(s_hero.icon, cond_icon(cond));
  lv_label_set_text(s_hero.cond, cond_label_fr(cond));

  char buf[64];
  // Max/min du jour = premier jour de la prévision (days[0]).
  if (f.valid && f.count > 0 && !std::isnan(f.days[0].temp_max))
    snprintf(buf, sizeof buf, "%.0f°", f.days[0].temp_max);
  else
    snprintf(buf, sizeof buf, "--");
  lv_label_set_text(s_hero.tmax, buf);
  if (f.valid && f.count > 0 && !std::isnan(f.days[0].temp_min))
    snprintf(buf, sizeof buf, "%.0f°", f.days[0].temp_min);
  else
    snprintf(buf, sizeof buf, "--");
  lv_label_set_text(s_hero.tmin, buf);

  // Ligne meta discrète : humidité + vent (le device n'a ni horloge ni
  // ressenti — pas de SNTP, pas de champ apparent_temperature côté HA).
  char h[16], w[16];
  if (c.valid && !std::isnan(c.humidity))
    snprintf(h, sizeof h, "%.0f%%", c.humidity);
  else
    snprintf(h, sizeof h, "--");
  if (c.valid && !std::isnan(c.wind_speed))
    snprintf(w, sizeof w, "%.0f km/h", c.wind_speed);
  else
    snprintf(w, sizeof w, "--");
  char meta[64];
  snprintf(meta, sizeof meta, "Humidité %s     ·     Vent %s", h, w);
  lv_label_set_text(s_hero.meta, meta);

  // --- Bande des 6 jours suivants (days[1..6]) ------------------------------
  for (int i = 0; i < 6; ++i) {
    DayCol &d = s_days[i];
    int idx = i + 1;
    if (!f.valid || idx >= f.count) {
      lv_label_set_text(d.day, "--");
      queue_icon_locked(d.icon, cond_icon(""));
      lv_label_set_text(d.tmax, "--");
      lv_label_set_text(d.tmin, "");
      lv_obj_set_style_bg_color(d.liser, temp_color(NAN), 0);
      continue;
    }
    const weather::DayForecast &wf = f.days[idx];
    lv_label_set_text(d.day, wf.day);
    queue_icon_locked(d.icon, cond_icon(wf.condition));
    lv_obj_set_style_bg_color(d.liser, temp_color(wf.temp_max), 0);
    if (!std::isnan(wf.temp_max))
      snprintf(buf, sizeof buf, "%.0f°", wf.temp_max);
    else
      snprintf(buf, sizeof buf, "--");
    lv_label_set_text(d.tmax, buf);
    if (!std::isnan(wf.temp_min)) {
      snprintf(buf, sizeof buf, "%.0f°", wf.temp_min);
      lv_label_set_text(d.tmin, buf);
    } else {
      lv_label_set_text(d.tmin, "");
    }
  }

  start_icon_stagger_locked();
}

// Contexte : tâche weather_fetch (PAS la tâche LVGL) -> LvglLock obligatoire.
static void on_fetch_done(bool ok) {
  bsp::Bsp::LvglLock lvgl_lock;
  if (ok) {
    lv_label_set_text(s_status, "");
    // Écran quitté entre-temps : refresh_locked() pouvait recréer des Lottie
    // (parse JSON + ThorVG) pendant un appel vidéo. SCREEN_LOADED le rejoue.
    if (!s_screen_active) {
      return;
    }
    refresh_locked();
  } else {
    lv_label_set_text(s_status, "Erreur météo");
  }
}

static void refresh_timer_cb(lv_timer_t *) {
  weather::fetch_async(on_fetch_done);
  int64_t start_s, end_s;
  agenda_range_local(start_s, end_s);
  gcal::fetch_async(start_s, end_s, on_gcal_fetch_done);
}

static void cb_screen_event(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_SCREEN_LOADED) {
    s_screen_active = true;
    if (!s_anim_timer) {
      s_anim_timer =
          lv_timer_create(anim_timer_cb, 1000 / lottie_frames::kFps, nullptr);
      lv_timer_pause(s_anim_timer);
    }
    s_icons_running = false; // état réel : timer créé en pause
    set_icons_running_locked(bsp::Bsp::instance().display_on());
    refresh_locked();
    update_clock_locked();
    if (weather::cache_stale(kCacheMaxAgeUs)) {
      lv_label_set_text(s_status, "Chargement...");
      weather::fetch_async(on_fetch_done);
    }
    {
      int64_t start_s, end_s;
      agenda_range_local(start_s, end_s);
      // cache_covers et non "cache pas périmé" : au retour de la page Agenda
      // le cache est tout frais mais rempli avec la semaine qu'elle affichait.
      // Frais ne veut pas dire pertinent — il faut refetcher la fenêtre du
      // jour. (update_events_locked filtre de toute façon, mais sans ce
      // refetch la ligne "Aujourd'hui" resterait vide jusqu'au timer.)
      if (gcal::cache_covers(start_s, end_s, kCacheMaxAgeUs))
        update_events_locked();
      else
        gcal::fetch_async(start_s, end_s, on_gcal_fetch_done);
    }
    if (!s_refresh_timer)
      s_refresh_timer =
          lv_timer_create(refresh_timer_cb, kRefreshPeriodMs, nullptr);
    if (!s_clock_timer)
      s_clock_timer = lv_timer_create(clock_timer_cb, kClockPeriodMs, nullptr);
    update_net_locked();
    if (!s_net_timer)
      s_net_timer = lv_timer_create(net_timer_cb, kNetPeriodMs, nullptr);
    update_missed_locked();
  } else if (code == LV_EVENT_SCREEN_UNLOADED) {
    s_screen_active = false;
    if (s_refresh_timer) {
      lv_timer_delete(s_refresh_timer);
      s_refresh_timer = nullptr;
    }
    if (s_icon_stagger_timer) {
      lv_timer_delete(s_icon_stagger_timer);
      s_icon_stagger_timer = nullptr;
    }
    if (s_clock_timer) {
      lv_timer_delete(s_clock_timer);
      s_clock_timer = nullptr;
    }
    if (s_net_timer) {
      lv_timer_delete(s_net_timer);
      s_net_timer = nullptr;
    }
    s_pending_count = 0;
    s_pending_pos = 0;
    if (s_anim_timer) {
      lv_timer_delete(s_anim_timer);
      s_anim_timer = nullptr;
    }
    s_icons_running = false;
    release_icons_locked();
  }
}

static void cb_back(lv_event_t *) { ui::load_home_screen(); }

// Conteneur flex transparent, sans fond/bordure/scroll : boîte d'aération.
static lv_obj_t *make_flex(lv_obj_t *parent, lv_flex_flow_t flow) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_set_style_radius(o, 0, 0);
  lv_obj_set_flex_flow(o, flow);
  lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  return o;
}

// Carte opaque : blanc cassé, coins 8px, filet 1px, PAS d'ombre (contrainte
// CPU — cf. entête). Renvoie l'objet, flex à configurer par l'appelant.
static lv_obj_t *make_card(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(o, lv_color_hex(kCard), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, 8, 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_border_color(o, lv_color_hex(kHair), 0);
  lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_width(o, 0, 0);
  ui::theme::card_frame(o);
  return o;
}

// Boîte de centrage de l'icône Lottie, d x d, sans fond ni bordure.
// S'appelait make_halo et peignait un disque plein derrière l'icône ; le
// remplissage a été retiré (le halo redessiné sous une animation ThorVG à
// chaque frame coûtait trop cher, cf. entête de fichier). La couleur du disque
// était encore passée en paramètre alors que plus personne ne la lisait : le
// paramètre est supprimé pour que la signature dise ce que la fonction fait.
static lv_obj_t *make_icon_box(lv_obj_t *parent, int32_t d) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(o, d, d);
  // Fond TRANSPARENT, et surtout pas « laissé au thème » : c'était le cas
  // jusqu'au passage du thème par défaut en mode sombre, où ces boîtes sont
  // devenues des carrés noirs derrière chaque icône — l'écran Météo est le seul
  // écran CLAIR de l'appli, et le seul endroit qui comptait encore sur la
  // couleur de carte du thème. Transparent plutôt que kCard : la boîte est
  // toujours posée sur une carte qui a déjà peint son fond, et un remplissage
  // de moins est un remplissage de moins sous une animation ThorVG.
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  return o;
}

// Pastille agenda : colonne (étiquette 24 px + lignes), fond plein, masquée
// jusqu'au premier événement connu.
static lv_obj_t *make_events_badge(uint32_t bg, uint32_t fg, lv_obj_t **tag) {
  lv_obj_t *b = make_flex(s_hero.events_col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_width(b, LV_PCT(100));
  lv_obj_set_height(b, LV_SIZE_CONTENT);
  lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(b, 10, 0);
  lv_obj_set_style_pad_hor(b, 14, 0);
  lv_obj_set_style_pad_ver(b, kEventsPad, 0);
  lv_obj_set_style_text_color(b, lv_color_hex(fg), 0);
  lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
  *tag = lv_label_create(b);
  lv_obj_set_style_text_font(*tag, &montserrat_fr_24, 0);
  lv_obj_set_style_text_opa(*tag, LV_OPA_80, 0);
  lv_label_set_text(*tag, "");
  return b;
}

static lv_obj_t *make_event_line(lv_obj_t *parent) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_width(l, LV_PCT(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(l, &montserrat_fr_36, 0);
  lv_label_set_text(l, "");
  return l;
}

void create_screen() {
  s_scr = lv_obj_create(NULL);
  lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s_scr, lv_color_hex(kBg), 0);
  ui::theme::screen_gradient(s_scr);
  lv_obj_set_style_pad_all(s_scr, 24, 0);
  lv_obj_set_flex_flow(s_scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(s_scr, 20, 0);
  lv_obj_add_event_cb(s_scr, cb_screen_event, LV_EVENT_SCREEN_LOADED, NULL);
  lv_obj_add_event_cb(s_scr, cb_screen_event, LV_EVENT_SCREEN_UNLOADED, NULL);

  ui_stats::attach(s_scr); // overlay FPS/CPU/RAM (bas-droite)

  // ===================== CARTE DU JOUR (~55% hauteur) =====================
  lv_obj_t *day_card = make_card(s_scr);
  lv_obj_set_width(day_card, LV_PCT(100));
  lv_obj_set_height(day_card, 440);
  lv_obj_set_style_pad_all(day_card, 28, 0);
  lv_obj_set_flex_flow(day_card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(day_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(day_card, 8, 0);

  // --- Entête : jour (gauche) + bouton retour (droite) ---
  lv_obj_t *head = make_flex(day_card, LV_FLEX_FLOW_ROW);
  lv_obj_set_width(head, LV_PCT(100));
  lv_obj_set_height(head, LV_SIZE_CONTENT);
  lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  // Bloc gauche : date puis éphéméride sur la même ligne. Alignement END (bas)
  // et non CENTER : les deux libellés n'ont pas la même taille de fonte, c'est
  // le pied de ligne commun qui les fait lire comme une seule phrase.
  lv_obj_t *hleft = make_flex(head, LV_FLEX_FLOW_ROW);
  // grow plutôt qu'une largeur en dur : le bloc prend TOUT ce que le bloc droit
  // (voyants + heure + retour) laisse, sans qu'on ait à en connaître la largeur.
  lv_obj_set_flex_grow(hleft, 1);
  lv_obj_set_height(hleft, LV_SIZE_CONTENT);
  lv_obj_set_flex_align(hleft, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                        LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(hleft, 16, 0);

  s_hero.date = lv_label_create(hleft);
  lv_obj_set_style_text_font(s_hero.date, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(s_hero.date, lv_color_hex(kInk), 0);

  // Éphéméride (saints du jour) : secondaire, donc gris et plus petit que la
  // date, mais pas au point d'être illisible de loin. Table locale, cf.
  // app/ephemeris.
  s_hero.saint = lv_label_create(hleft);
  lv_obj_set_style_text_font(s_hero.saint, &montserrat_fr_24, 0);
  lv_obj_set_style_text_color(s_hero.saint, lv_color_hex(kSub), 0);
  // Occupe tout le reste de la ligne, et coupe en "…" plutôt que de pousser le
  // bloc droit : les jours à trois saints dépassent la place en 24 px.
  lv_obj_set_flex_grow(s_hero.saint, 1);
  lv_label_set_long_mode(s_hero.saint, LV_LABEL_LONG_DOT);
  lv_label_set_text(s_hero.saint, "");

  // Bloc droit : badge heure + bouton retour.
  lv_obj_t *hright = make_flex(head, LV_FLEX_FLOW_ROW);
  lv_obj_set_width(hright, LV_SIZE_CONTENT);
  lv_obj_set_height(hright, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_column(hright, 12, 0);

  // Indicateurs de lien, à gauche du badge heure : dans un conteneur flex,
  // l'ordre de création est l'ordre d'affichage.
  s_net_wifi = lv_label_create(hright);
  lv_label_set_text(s_net_wifi, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_font(s_net_wifi, &lv_font_montserrat_24, 0);

  s_net_ha = lv_label_create(hright);
  lv_label_set_text(s_net_ha, LV_SYMBOL_CHARGE);
  lv_obj_set_style_text_font(s_net_ha, &lv_font_montserrat_24, 0);

  update_net_locked();

  // Badge heure : fond gris clair arrondi.
  lv_obj_t *clock_badge = lv_obj_create(hright);
  lv_obj_remove_flag(clock_badge, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_height(clock_badge, 72);
  lv_obj_set_width(clock_badge, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(clock_badge, lv_color_hex(kBadge), 0);
  lv_obj_set_style_bg_opa(clock_badge, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(clock_badge, 16, 0);
  lv_obj_set_style_border_width(clock_badge, 1, 0);
  lv_obj_set_style_border_color(clock_badge, lv_color_hex(kHair), 0);
  lv_obj_set_style_pad_hor(clock_badge, 20, 0);
  lv_obj_set_style_pad_ver(clock_badge, 0, 0);
  s_hero.clock = lv_label_create(clock_badge);
  lv_obj_center(s_hero.clock);
  lv_obj_set_style_text_font(s_hero.clock, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(s_hero.clock, lv_color_hex(kInk), 0);
  lv_label_set_text(s_hero.clock, "--:--");

  // Bouton retour : carré arrondi clair, flèche gauche.
  lv_obj_t *back_btn = lv_button_create(hright);
  lv_obj_set_size(back_btn, 72, 72);
  lv_obj_set_style_bg_color(back_btn, lv_color_hex(kBadge), 0);
  lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(back_btn, 16, 0);
  lv_obj_set_style_border_width(back_btn, 1, 0);
  lv_obj_set_style_border_color(back_btn, lv_color_hex(kHair), 0);
  lv_obj_set_style_shadow_width(back_btn, 0, 0);
  lv_obj_t *back_lbl = lv_label_create(back_btn);
  lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(back_lbl, lv_color_hex(kInk), 0);
  lv_obj_center(back_lbl);
  lv_obj_add_event_cb(back_btn, cb_back, LV_EVENT_CLICKED, NULL);

  // --- Corps : météo (gauche) · | · agenda pleine hauteur (droite) ---
  // L'agenda prend la place des anciens max/min ET toute la hauteur sous
  // l'entête : c'est la seule façon de lister les événements du jour sans
  // plafond arbitraire (cf. update_events_locked).
  lv_obj_t *body = make_flex(day_card, LV_FLEX_FLOW_ROW);
  lv_obj_set_width(body, LV_PCT(100));
  lv_obj_set_flex_grow(body, 1);
  lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(body, 28, 0);

  lv_obj_t *wx = make_flex(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_size(wx, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_align(wx, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(wx, 2, 0);

  // Icône(halo) · max XXL · min. La température actuelle a disparu : sur un
  // écran mural, c'est la fourchette de la journée qui sert à s'habiller.
  lv_obj_t *main = make_flex(wx, LV_FLEX_FLOW_ROW);
  lv_obj_set_size(main, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_align(main, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(main, 20, 0);

  lv_obj_t *icon_box = make_icon_box(main, 180);
  s_hero.icon.px = 180;
  s_hero.icon.widget = lv_image_create(icon_box);
  lv_obj_center(s_hero.icon.widget);
  s_all_icons[0] = &s_hero.icon;

  // Max plus gros que min, alignés sur le pied : le max est la valeur lue en
  // premier. 144/112 et non 210 : en 210 la ligne seule (~255 px) dépassait la
  // hauteur du corps une fois la condition posée dessous.
  lv_obj_t *mmblk = make_flex(main, LV_FLEX_FLOW_ROW);
  lv_obj_set_size(mmblk, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_align(mmblk, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                        LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(mmblk, 20, 0);
  s_hero.tmax = lv_label_create(mmblk);
  lv_obj_set_style_text_font(s_hero.tmax, &montserrat_fr_144, 0);
  lv_obj_set_style_text_color(s_hero.tmax, lv_color_hex(kWarm), 0);
  s_hero.tmin = lv_label_create(mmblk);
  lv_obj_set_style_text_font(s_hero.tmin, &montserrat_fr_112, 0);
  lv_obj_set_style_text_color(s_hero.tmin, lv_color_hex(kCold), 0);

  s_hero.cond = lv_label_create(wx);
  lv_obj_set_style_text_font(s_hero.cond, &montserrat_fr_64, 0);
  lv_obj_set_style_text_color(s_hero.cond, lv_color_hex(kInk), 0);
  // 24 et non 36 : la colonne gauche doit tenir dans la hauteur du corps
  // (icône 180 + condition 78 + meta).
  s_hero.meta = lv_label_create(wx);
  lv_obj_set_style_text_font(s_hero.meta, &montserrat_fr_24, 0);
  lv_obj_set_style_text_color(s_hero.meta, lv_color_hex(kSub), 0);

  // Séparateur vertical fin.
  lv_obj_t *sep = lv_obj_create(body);
  lv_obj_remove_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(sep, 2, LV_PCT(90));
  lv_obj_set_style_radius(sep, 1, 0);
  lv_obj_set_style_bg_color(sep, lv_color_hex(kHair), 0);
  lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(sep, 0, 0);

  // Colonne agenda : pastille du jour (kWarm) puis prochain événement (orange
  // clair, pour distinguer d'un coup d'œil aujourd'hui de plus tard). Les deux
  // masquées tant que rien n'est connu.
  s_hero.events_col = make_flex(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_grow(s_hero.events_col, 1);
  lv_obj_set_height(s_hero.events_col, LV_PCT(100));
  lv_obj_set_flex_align(s_hero.events_col, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(s_hero.events_col, kEventsGap, 0);

  s_hero.today_badge = make_events_badge(kWarm, 0xFFFFFF, &s_hero.today_tag);
  lv_label_set_text(s_hero.today_tag, "AUJOURD'HUI");
  for (lv_obj_t *&line : s_hero.today_lines) {
    line = make_event_line(s_hero.today_badge);
    lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
  }

  s_hero.next_badge = make_events_badge(kNextBg, kNextInk, &s_hero.next_tag);
  s_hero.next_line = make_event_line(s_hero.next_badge);
  lv_obj_set_height(s_hero.next_line,
                    lv_font_get_line_height(&montserrat_fr_36));

  // ===================== BANDE PRÉVISIONS (6 jours) =======================
  lv_obj_t *week = make_flex(s_scr, LV_FLEX_FLOW_ROW);
  lv_obj_set_width(week, LV_PCT(100));
  lv_obj_set_flex_grow(week, 1);
  lv_obj_set_flex_align(week, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(week, 16, 0);

  for (int i = 0; i < 6; ++i) {
    lv_obj_t *card = make_card(week);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_height(card, LV_PCT(100));
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_top(card, 18, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    DayCol &d = s_days[i];

    // Liseré coloré fin en haut (barre pleine, couleur = palier température).
    d.liser = lv_obj_create(card);
    lv_obj_remove_flag(d.liser, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(d.liser, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(d.liser, LV_PCT(60), 4);
    lv_obj_align(d.liser, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(d.liser, 2, 0);
    lv_obj_set_style_border_width(d.liser, 0, 0);
    lv_obj_set_style_bg_color(d.liser, lv_color_hex(kHair), 0);
    lv_obj_set_style_bg_opa(d.liser, LV_OPA_COVER, 0);

    d.day = lv_label_create(card);
    lv_obj_set_style_text_font(d.day, &montserrat_fr_36, 0);
    lv_obj_set_style_text_color(d.day, lv_color_hex(kSub), 0);

    // 120 px (et non plus 96) : le coût ThorVG croît au carré de la taille, la
    // bande hebdo ne laisse que ~260 px utiles en hauteur (libellé jour ~44 +
    // halo + max/min ~78) — 132 de halo est le plafond avant compression.
    lv_obj_t *dbox = make_icon_box(card, 132);
    d.icon.px = 120;
    d.icon.widget = lv_image_create(dbox);
    lv_obj_center(d.icon.widget);
    s_all_icons[i + 1] = &d.icon;

    // Max (orange) / min (bleu) côte à côte.
    lv_obj_t *mm = make_flex(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(mm, LV_SIZE_CONTENT);
    lv_obj_set_height(mm, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(mm, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(mm, 8, 0);
    d.tmax = lv_label_create(mm);
    lv_obj_set_style_text_font(d.tmax, &montserrat_fr_64, 0);
    lv_obj_set_style_text_color(d.tmax, lv_color_hex(kWarm), 0);
    d.tmin = lv_label_create(mm);
    lv_obj_set_style_text_font(d.tmin, &montserrat_fr_36, 0);
    lv_obj_set_style_text_color(d.tmin, lv_color_hex(kCold), 0);
  }

  // Statut discret (haut, hors flux) — n'occupe pas d'espace de layout.
  s_status = lv_label_create(s_scr);
  lv_obj_add_flag(s_status, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_label_set_text(s_status, "");
  lv_obj_set_style_text_font(s_status, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(s_status, lv_color_hex(ui::theme::active.warn), 0);
  lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 4);

  // ===================== NOTIFICATION D'APPELS MANQUÉS =====================
  // Créée en dernier = dessinée par-dessus les cartes. IGNORE_LAYOUT : le flex
  // colonne de l'écran ne doit ni la placer ni lui réserver de hauteur.
  s_missed_box = lv_obj_create(s_scr);
  lv_obj_add_flag(s_missed_box, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_remove_flag(s_missed_box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(s_missed_box, 700, 300);
  lv_obj_center(s_missed_box);
  lv_obj_set_style_bg_color(s_missed_box, lv_color_hex(kCard), 0);
  lv_obj_set_style_bg_opa(s_missed_box, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_missed_box, 16, 0);
  lv_obj_set_style_border_width(s_missed_box, 3, 0);
  lv_obj_set_style_border_color(s_missed_box, lv_color_hex(kWarm), 0);
  lv_obj_set_style_border_opa(s_missed_box, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_width(s_missed_box, 0, 0); // cf. contrainte CPU
  lv_obj_set_style_pad_all(s_missed_box, 24, 0);
  lv_obj_set_flex_flow(s_missed_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_missed_box, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(s_missed_box, LV_OBJ_FLAG_HIDDEN);

  s_missed_lbl = lv_label_create(s_missed_box);
  lv_obj_set_style_text_font(s_missed_lbl, &montserrat_fr_64, 0);
  lv_obj_set_style_text_color(s_missed_lbl, lv_color_hex(kInk), 0);
  lv_label_set_text(s_missed_lbl, "");

  lv_obj_t *mrow = make_flex(s_missed_box, LV_FLEX_FLOW_ROW);
  lv_obj_set_width(mrow, LV_PCT(100));
  lv_obj_set_height(mrow, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_column(mrow, 24, 0);

  lv_obj_t *view_btn = lv_button_create(mrow);
  lv_obj_set_size(view_btn, 280, 88);
  lv_obj_set_style_bg_color(view_btn, lv_color_hex(kWarm), 0);
  lv_obj_set_style_bg_opa(view_btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(view_btn, 16, 0);
  lv_obj_set_style_shadow_width(view_btn, 0, 0);
  lv_obj_t *view_lbl = lv_label_create(view_btn);
  lv_label_set_text(view_lbl, "Voir");
  lv_obj_set_style_text_font(view_lbl, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(view_lbl, lv_color_white(), 0);
  lv_obj_center(view_lbl);
  lv_obj_add_event_cb(view_btn, cb_missed_view, LV_EVENT_CLICKED, NULL);

  lv_obj_t *close_btn = lv_button_create(mrow);
  lv_obj_set_size(close_btn, 280, 88);
  lv_obj_set_style_bg_color(close_btn, lv_color_hex(kBadge), 0);
  lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(close_btn, 16, 0);
  lv_obj_set_style_border_width(close_btn, 1, 0);
  lv_obj_set_style_border_color(close_btn, lv_color_hex(kHair), 0);
  lv_obj_set_style_shadow_width(close_btn, 0, 0);
  lv_obj_t *close_lbl = lv_label_create(close_btn);
  lv_label_set_text(close_lbl, "Fermer");
  lv_obj_set_style_text_font(close_lbl, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(close_lbl, lv_color_hex(kInk), 0);
  lv_obj_center(close_lbl);
  lv_obj_add_event_cb(close_btn, cb_missed_close, LV_EVENT_CLICKED, NULL);

  refresh_locked();      // placeholders initiaux
  update_clock_locked(); // "--" tant que SNTP pas synchronisé
  ESP_LOGD(TAG, "écran météo V3 créé");
}

void open_screen() {
  // Sans animation : cf. ui::load_home_screen(). Un slide de 300 ms ne tient
  // que 3 à 6 repaints pleins sur ce panneau, et l'écran météo est le plus
  // chargé de l'appli (8 Lottie + 7 colonnes) — chaque étape y coûte le plein
  // tarif.
  lv_scr_load(s_scr);
}

bool is_active() { return s_scr != nullptr && lv_screen_active() == s_scr; }

} // namespace ui_weather_v3
