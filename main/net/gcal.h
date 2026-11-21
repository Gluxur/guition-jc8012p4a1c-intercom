#pragma once

#include <cstdint>

namespace gcal {

struct Event {
  int64_t start_epoch_s;
  int64_t end_epoch_s;
  bool all_day;
  char summary[48]; // "" si absent ; tronqué si plus long
};

struct EventList {
  // 64 et non 48 : la grille mois fait 42 jours (6 semaines pleines, débords
  // des mois voisins compris), pas 28-31 — à densité d'événements égale il
  // fallait remonter le plafond d'autant, sinon la fin de grille se vide.
  // ~4,6 Ko : la structure est COPIÉE sur la pile de gcal_fetch et de
  // lvgl_task, ne pas la gonfler sans relire le dimensionnement de piles dans
  // gcal.cpp (fetch_async).
  static constexpr int kMax = 64;
  Event events[kMax];
  int count = 0;
  bool valid = false;
  int64_t range_start_s = 0;
  int64_t range_end_s = 0;
  int64_t fetched_at_us = 0;
};

/* Parse {"response": [...]} (calendar.get_events reshaped by gcal.cpp's
 * template, cf. gcal_parse.cpp) : chaque élément a
 * "summary" (optionnel), "start"/"end" avec soit "dateTime" (RFC3339,
 * horaire) soit "date" (YYYY-MM-DD, journée entière -> all_day=true).
 * Trie le résultat par start_epoch_s croissant (l'ordre HA n'est pas
 * garanti). Tronque à kMax événements. false si JSON invalide. Défini dans
 * gcal_parse.cpp (logique pure, testée sur host : test/test_gcal_parse.cpp). */
bool parse_events_json(const char *json, EventList &out);

/* Minuit UTC de la date civile (y, mo, d) — mo et d comptés à partir de 1.
 * Sert aux appelants pour construire l'argument `utc_day_start_s` ci-dessous
 * sans réimplémenter l'algorithme calendaire. Défini dans gcal_parse.cpp. */
[[nodiscard]] int64_t utc_midnight(int y, int mo, int d);

/* Écrit dans `out` (au plus `max_out`) les pointeurs vers les événements de `l`
 * qui CHEVAUCHENT le jour décrit par les bornes, dans l'ordre du tableau (déjà
 * trié chronologiquement par parse_events_json). Retourne le nombre écrit.
 *
 * Chevauchement et non « commence dans la fenêtre » : un événement de plusieurs
 * jours doit apparaître sur CHACUN de ses jours, pas seulement le premier.
 *
 * DEUX fenêtres, parce que les deux familles d'événements ne vivent pas dans le
 * même repère de temps :
 *  - `[day_start_s, day_end_s)` = minuit local -> minuit local (le paramètre est
 *    séparé de day_start+86400 pour les jours de changement d'heure, 23 ou 25 h)
 *    -> événements horaires, dont l'epoch est un instant réel.
 *  - `utc_day_start_s` = minuit UTC de la MÊME date civile (cf. utc_midnight)
 *    -> événements journée entière : le JSON ne porte qu'une date, le parseur
 *    l'ancre à minuit UTC. Les comparer à la fenêtre locale les ferait déborder
 *    du décalage du fuseau sur le jour suivant (+2 h en France l'été).
 * Fenêtres semi-ouvertes : un événement finissant à minuit pile n'appartient pas
 * au jour qui commence.
 *
 * À utiliser par TOUT afficheur : le cache est global et partagé entre la page
 * Météo (fenêtre du jour) et la page Agenda (semaine/mois navigable). Se fier à
 * la fenêtre demandée au dernier fetch au lieu de filtrer donne les événements
 * d'une autre période. Les pointeurs restent valides tant que `l` vit.
 * Défini dans gcal_parse.cpp. */
int events_on_day(const EventList &l, int64_t day_start_s, int64_t day_end_s,
                  int64_t utc_day_start_s, const Event **out, int max_out);

/* true si `e` a commencé AVANT le jour décrit par ces bornes (mêmes repères que
 * events_on_day) : l'UI affiche alors une marque de continuation au lieu d'une
 * heure de début qui n'a pas lieu ce jour-là. Défini dans gcal_parse.cpp. */
[[nodiscard]] bool starts_before_day(const Event &e, int64_t day_start_s,
                                     int64_t utc_day_start_s);

/* true si `l` est valide, couvre entièrement [start_s, end_s) et date de moins
 * de `max_age_us`. Prédicat pur (l'horloge est passée en paramètre) : c'est le
 * cœur testable de cache_covers(). Défini dans gcal_parse.cpp. */
[[nodiscard]] bool range_covers(const EventList &l, int64_t start_s,
                                int64_t end_s, int64_t now_us,
                                int64_t max_age_us);

/* Copie du cache, prise sous verrou (même raisonnement que weather::cached :
 * une référence exposerait une lecture déchirée entre la tâche gcal_fetch et
 * la tâche LVGL). */
[[nodiscard]] EventList cached();

/* range_covers() appliqué au cache, sous verrou. PAS de simple `cache_stale` :
 * la fraîcheur seule ne dit rien de la PÉRIODE en cache, et celle-ci dépend de
 * qui a fetché en dernier (Météo = le jour, Agenda = la semaine/le mois
 * affiché). Un cache tout frais rempli par l'Agenda sur une autre semaine ne
 * doit pas servir à l'écran Météo. */
[[nodiscard]] bool cache_covers(int64_t start_s, int64_t end_s,
                                int64_t max_age_us);

/* calendar.get_events on cfg::kHaCalendarEntity through the ESPHome API (no
 * token), bounds formatted in UTC. Lance l'appel + parse dans une tâche
 * FreeRTOS courte auto-détruite ; done_cb(ok) est appelé depuis cette tâche
 * (LvglLock requis avant tout lv_* dans le callback). Ignoré si un fetch est
 * déjà en cours. */
void fetch_async(int64_t range_start_s, int64_t range_end_s,
                 void (*done_cb)(bool ok));

} // namespace gcal
