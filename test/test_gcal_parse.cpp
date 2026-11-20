#include "net/gcal.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

namespace {

// Époques de référence calculées À LA MAIN, pas produites par le parser :
// 2026-01-01T00:00:00Z = 1767225600 (vérifié par 1577836800 pour 2020-01-01
// + 2192 jours). 2026 n'est pas bissextile, le 30 juillet est le 210e jour
// après le 1er janvier -> 1767225600 + 210*86400.
constexpr int64_t kJul30Midnight = 1'785'369'600;
constexpr int64_t kJul31Midnight = kJul30Midnight + 86400;

gcal::EventList parse(const std::string &json, bool expect_ok = true) {
  gcal::EventList l;
  const bool ok = gcal::parse_events_json(json.c_str(), l);
  EXPECT_EQ(expect_ok, ok);
  return l;
}

std::string timed_event(const char *summary, const char *start,
                        const char *end) {
  std::string s = "{";
  if (summary && *summary) {
    s += "\"summary\":\"";
    s += summary;
    s += "\",";
  }
  s += "\"start\":{\"dateTime\":\"";
  s += start;
  s += "\"},\"end\":{\"dateTime\":\"";
  s += end;
  s += "\"}}";
  return s;
}

// --- Entrées malformées -----------------------------------------------------

TEST(GcalParse, RejectsMalformedJson) {
  gcal::EventList l;
  EXPECT_FALSE(gcal::parse_events_json("not json", l));
}

TEST(GcalParse, RejectsAnObjectAtTheRoot) {
  // HA renvoie un tableau ; un objet, c'est typiquement une page d'erreur JSON
  // ({"message":"Entity not found"}) qui ne doit pas passer pour un agenda vide.
  gcal::EventList l;
  EXPECT_FALSE(gcal::parse_events_json("{\"message\":\"Entity not found\"}", l));
}

TEST(GcalParse, AcceptsAnEmptyCalendar) {
  // Distinction importante côté UI : "aucun événement" est un succès, pas une
  // panne de fetch.
  const gcal::EventList l = parse("[]");
  EXPECT_EQ(0, l.count);
}

TEST(GcalParse, OutputIsResetEvenWhenParsingFails) {
  // La sortie est réutilisée d'un fetch à l'autre : un échec ne doit pas
  // laisser en place les événements du fetch précédent.
  gcal::EventList l = parse("[" + timed_event("Vieux", "2026-07-30T08:00:00Z",
                                              "2026-07-30T09:00:00Z") +
                            "]");
  ASSERT_EQ(1, l.count);
  EXPECT_FALSE(gcal::parse_events_json("not json", l));
  EXPECT_EQ(0, l.count);
}

// --- Fuseaux horaires -------------------------------------------------------

TEST(GcalParse, AppliesAPositiveUtcOffset) {
  // 14:30 en +02:00 = 12:30 UTC. Un signe inversé décalerait l'agenda de 4 h.
  const gcal::EventList l =
      parse("[" +
            timed_event("Reunion", "2026-07-30T14:30:00+02:00",
                        "2026-07-30T15:00:00+02:00") +
            "]");
  ASSERT_EQ(1, l.count);
  EXPECT_EQ(kJul30Midnight + 12 * 3600 + 30 * 60, l.events[0].start_epoch_s);
  EXPECT_EQ(kJul30Midnight + 13 * 3600, l.events[0].end_epoch_s);
}

TEST(GcalParse, AppliesANegativeUtcOffset) {
  const gcal::EventList l =
      parse("[" +
            timed_event("Call US", "2026-07-30T09:00:00-05:00",
                        "2026-07-30T10:00:00-05:00") +
            "]");
  ASSERT_EQ(1, l.count);
  EXPECT_EQ(kJul30Midnight + 14 * 3600, l.events[0].start_epoch_s);
}

TEST(GcalParse, TreatsTheZSuffixAsUtc) {
  const gcal::EventList l =
      parse("[" +
            timed_event("UTC", "2026-07-30T08:00:00Z", "2026-07-30T09:00:00Z") +
            "]");
  ASSERT_EQ(1, l.count);
  EXPECT_EQ(kJul30Midnight + 8 * 3600, l.events[0].start_epoch_s);
}

TEST(GcalParse, HandlesAHalfHourOffset) {
  // +05:30 (Inde) : les minutes du décalage comptent, pas seulement les heures.
  const gcal::EventList l =
      parse("[" +
            timed_event("IST", "2026-07-30T12:00:00+05:30",
                        "2026-07-30T13:00:00+05:30") +
            "]");
  ASSERT_EQ(1, l.count);
  EXPECT_EQ(kJul30Midnight + 6 * 3600 + 30 * 60, l.events[0].start_epoch_s);
}

TEST(GcalParse, TreatsAMissingOffsetAsUtc) {
  const gcal::EventList l =
      parse("[" +
            timed_event("Naif", "2026-07-30T08:00:00", "2026-07-30T09:00:00") +
            "]");
  ASSERT_EQ(1, l.count);
  EXPECT_EQ(kJul30Midnight + 8 * 3600, l.events[0].start_epoch_s);
}

// --- Journées entières ------------------------------------------------------

TEST(GcalParse, FlagsDateOnlyEventsAsAllDay) {
  const gcal::EventList l =
      parse("[{\"summary\":\"Vacances\",\"start\":{\"date\":\"2026-07-30\"},"
            "\"end\":{\"date\":\"2026-07-31\"}}]");
  ASSERT_EQ(1, l.count);
  EXPECT_TRUE(l.events[0].all_day);
  EXPECT_EQ(kJul30Midnight, l.events[0].start_epoch_s);
  EXPECT_EQ(kJul31Midnight, l.events[0].end_epoch_s);
}

TEST(GcalParse, TimedEventsAreNotFlaggedAllDay) {
  const gcal::EventList l =
      parse("[" +
            timed_event("RDV", "2026-07-30T08:00:00Z", "2026-07-30T09:00:00Z") +
            "]");
  ASSERT_EQ(1, l.count);
  EXPECT_FALSE(l.events[0].all_day);
}

// --- Tri --------------------------------------------------------------------

TEST(GcalParse, SortsChronologicallyWhateverTheInputOrder) {
  // L'intégration Google Calendar de HA ne garantit pas l'ordre : sans ce tri
  // l'agenda affiche l'après-midi avant le matin.
  const gcal::EventList l =
      parse("[" +
            timed_event("Soir", "2026-07-30T20:00:00Z", "2026-07-30T21:00:00Z") +
            "," +
            timed_event("Matin", "2026-07-30T08:00:00Z",
                        "2026-07-30T09:00:00Z") +
            "," +
            timed_event("Midi", "2026-07-30T12:00:00Z", "2026-07-30T13:00:00Z") +
            "]");
  ASSERT_EQ(3, l.count);
  EXPECT_STREQ("Matin", l.events[0].summary);
  EXPECT_STREQ("Midi", l.events[1].summary);
  EXPECT_STREQ("Soir", l.events[2].summary);
}

TEST(GcalParse, SortIsStableEnoughToKeepBothSimultaneousEvents) {
  // Deux événements à la même heure : aucun ne doit disparaître ni écraser
  // l'autre (le tri par insertion recopie des structures entières).
  const gcal::EventList l =
      parse("[" + timed_event("A", "2026-07-30T08:00:00Z",
                              "2026-07-30T09:00:00Z") +
            "," +
            timed_event("B", "2026-07-30T08:00:00Z", "2026-07-30T09:00:00Z") +
            "]");
  ASSERT_EQ(2, l.count);
  EXPECT_EQ(l.events[0].start_epoch_s, l.events[1].start_epoch_s);
  const std::string a = l.events[0].summary, b = l.events[1].summary;
  EXPECT_NE(a, b);
}

// --- Champs partiels --------------------------------------------------------

TEST(GcalParse, LeavesTheSummaryEmptyWhenAbsent) {
  // Cas courant des journées entières importées : l'UI affiche un libellé de
  // repli, elle ne doit pas lire une chaîne non initialisée.
  const gcal::EventList l =
      parse("[{\"start\":{\"date\":\"2026-07-30\"},"
            "\"end\":{\"date\":\"2026-07-31\"}}]");
  ASSERT_EQ(1, l.count);
  EXPECT_STREQ("", l.events[0].summary);
}

TEST(GcalParse, TruncatesAnOverlongSummaryAndKeepsItTerminated) {
  // summary[48] : un titre d'événement plus long doit être coupé proprement,
  // pas déborder sur les champs voisins de la structure.
  const std::string longue(120, 'x');
  const gcal::EventList l =
      parse("[" + timed_event(longue.c_str(), "2026-07-30T08:00:00Z",
                              "2026-07-30T09:00:00Z") +
            "]");
  ASSERT_EQ(1, l.count);
  EXPECT_EQ(sizeof(gcal::Event::summary) - 1,
            std::string(l.events[0].summary).size());
}

TEST(GcalParse, FallsBackToStartWhenEndIsMissing) {
  const gcal::EventList l =
      parse("[{\"summary\":\"Sans fin\",\"start\":{\"dateTime\":"
            "\"2026-07-30T08:00:00Z\"}}]");
  ASSERT_EQ(1, l.count);
  EXPECT_EQ(l.events[0].start_epoch_s, l.events[0].end_epoch_s);
}

TEST(GcalParse, FallsBackToStartWhenEndIsUnparseable) {
  const gcal::EventList l =
      parse("[" +
            timed_event("Fin cassee", "2026-07-30T08:00:00Z", "n'importe quoi") +
            "]");
  ASSERT_EQ(1, l.count);
  EXPECT_EQ(l.events[0].start_epoch_s, l.events[0].end_epoch_s);
}

// --- Événements rejetés -----------------------------------------------------

TEST(GcalParse, SkipsEventsWithoutAStart) {
  // Le slot de l'événement rejeté doit être RÉUTILISÉ par le suivant, sinon
  // l'agenda affiche un trou avec des données résiduelles.
  const gcal::EventList l =
      parse("[{\"summary\":\"Orphelin\"},"
            "" +
            timed_event("Valide", "2026-07-30T08:00:00Z",
                        "2026-07-30T09:00:00Z") +
            "]");
  ASSERT_EQ(1, l.count);
  EXPECT_STREQ("Valide", l.events[0].summary);
}

TEST(GcalParse, SkipsEventsWithAnUnparseableStart) {
  const gcal::EventList l =
      parse("[{\"summary\":\"Cassé\",\"start\":{\"dateTime\":\"hier\"}}," +
            timed_event("Valide", "2026-07-30T08:00:00Z",
                        "2026-07-30T09:00:00Z") +
            "]");
  ASSERT_EQ(1, l.count);
  EXPECT_STREQ("Valide", l.events[0].summary);
}

TEST(GcalParse, SkipsEventsWhoseStartIsNeitherDateTimeNorDate) {
  const gcal::EventList l =
      parse("[{\"summary\":\"Bizarre\",\"start\":{\"epoch\":123}}," +
            timed_event("Valide", "2026-07-30T08:00:00Z",
                        "2026-07-30T09:00:00Z") +
            "]");
  ASSERT_EQ(1, l.count);
  EXPECT_STREQ("Valide", l.events[0].summary);
}

// --- Débordement ------------------------------------------------------------

TEST(GcalParse, TruncatesAtTheEventCapWithoutOverflowing) {
  // EventList::kMax = 48 : une fenêtre chargée doit être coupée, jamais écrire
  // hors du tableau (le parser tourne sur une structure de ~3,3 Ko en pile de
  // tâche).
  std::string json = "[";
  for (int i = 0; i < gcal::EventList::kMax + 12; ++i) {
    char start[32], end[32], name[16];
    // Heures croissantes sur plusieurs jours pour rester un RFC3339 valide.
    std::snprintf(start, sizeof start, "2026-07-%02dT%02d:00:00Z",
                  1 + i / 12, i % 12);
    std::snprintf(end, sizeof end, "2026-07-%02dT%02d:30:00Z", 1 + i / 12,
                  i % 12);
    std::snprintf(name, sizeof name, "E%d", i);
    if (i)
      json += ",";
    json += timed_event(name, start, end);
  }
  json += "]";

  const gcal::EventList l = parse(json);
  EXPECT_EQ(gcal::EventList::kMax, l.count);
  // Les 48 retenus sont les 48 PREMIERS du flux (troncature, pas sélection),
  // et ils restent triés.
  for (int i = 1; i < l.count; ++i)
    ASSERT_LE(l.events[i - 1].start_epoch_s, l.events[i].start_epoch_s)
        << "i=" << i;
}

// --- Filtrage par fenêtre ---------------------------------------------------
// Le cache gcal est GLOBAL et partagé : la page Météo veut la fenêtre du jour,
// la page Agenda une semaine ou un mois navigables. Le dernier fetch peut donc
// venir de l'autre page — d'où ce filtre côté affichage.

gcal::EventList three_days() {
  return parse("[" +
               timed_event("Hier", "2026-07-29T10:00:00Z",
                           "2026-07-29T11:00:00Z") +
               "," +
               timed_event("Matin", "2026-07-30T08:00:00Z",
                           "2026-07-30T09:00:00Z") +
               "," +
               timed_event("Soir", "2026-07-30T20:00:00Z",
                           "2026-07-30T21:00:00Z") +
               "," +
               timed_event("Demain", "2026-07-31T10:00:00Z",
                           "2026-07-31T11:00:00Z") +
               "]");
}

// Les deux pages appellent avec un jour LOCAL ; ici, sauf test de fuseau
// explicite, local == UTC (la fenêtre du jour et son ancre journée-entière
// coïncident).
int events_on_utc_day(const gcal::EventList &l, int64_t day_s,
                      const gcal::Event **out, int max_out) {
  return gcal::events_on_day(l, day_s, day_s + 86400, day_s, out, max_out);
}

std::string all_day_event(const char *summary, const char *start_date,
                          const char *end_date) {
  std::string s = "{\"summary\":\"";
  s += summary;
  s += "\",\"start\":{\"date\":\"";
  s += start_date;
  s += "\"},\"end\":{\"date\":\"";
  s += end_date;
  s += "\"}}";
  return s;
}

TEST(GcalRange, KeepsOnlyEventsOverlappingTheWindow) {
  const gcal::EventList l = three_days();
  const gcal::Event *out[8] = {};
  const int n = events_on_utc_day(l, kJul30Midnight, out, 8);
  ASSERT_EQ(2, n);
  EXPECT_STREQ("Matin", out[0]->summary);
  EXPECT_STREQ("Soir", out[1]->summary);
}

TEST(GcalRange, WindowIsHalfOpen) {
  // [start, end) : minuit pile appartient au jour qui commence, pas au
  // précédent — sinon un événement à 00:00 s'affiche deux jours de suite.
  const gcal::EventList l =
      parse("[" +
            timed_event("Minuit", "2026-07-31T00:00:00Z",
                        "2026-07-31T01:00:00Z") +
            "]");
  const gcal::Event *out[4] = {};
  EXPECT_EQ(0, events_on_utc_day(l, kJul30Midnight, out, 4));
  EXPECT_EQ(1, events_on_utc_day(l, kJul31Midnight, out, 4));
}

TEST(GcalRange, ReturnsNothingWhenTheCacheHoldsAnotherPeriod) {
  // Le bug corrigé : Agenda navigue sur une autre semaine, remplit le cache,
  // puis la page Météo affichait ce contenu comme "aujourd'hui".
  const gcal::EventList l = three_days();
  const gcal::Event *out[8] = {};
  const int64_t next_week = kJul30Midnight + 7 * 86400;
  EXPECT_EQ(0, events_on_utc_day(l, next_week, out, 8));
}

TEST(GcalRange, RespectsTheOutputCap) {
  const gcal::EventList l = three_days();
  const gcal::Event *out[1] = {};
  ASSERT_EQ(1, events_on_utc_day(l, kJul30Midnight, out, 1));
  // Le plafond garde les PREMIERS de la fenêtre (liste déjà triée), pas les
  // derniers : la page Météo n'affiche que 3 lignes, ce doit être les 3
  // prochaines.
  EXPECT_STREQ("Matin", out[0]->summary);
}

TEST(GcalRange, HandlesAnEmptyCacheAndAZeroCap) {
  gcal::EventList empty;
  const gcal::Event *out[4] = {};
  EXPECT_EQ(0, events_on_utc_day(empty, kJul30Midnight, out, 4));
  const gcal::EventList l = three_days();
  EXPECT_EQ(0, events_on_utc_day(l, kJul30Midnight, out, 0));
}

// --- Événements sur plusieurs jours -----------------------------------------
// Le bug : le filtre ne regardait que le DÉBUT, un séjour du 30 au 2 ne
// s'affichait que sur la colonne du 30 (vues semaine et mois de l'Agenda).

TEST(GcalRange, ShowsATimedEventOnEveryDayItSpans) {
  const gcal::EventList l =
      parse("[" +
            timed_event("Voyage", "2026-07-30T18:00:00Z",
                        "2026-08-01T09:00:00Z") +
            "]");
  const gcal::Event *out[4] = {};
  EXPECT_EQ(1, events_on_utc_day(l, kJul30Midnight, out, 4));
  EXPECT_EQ(1, events_on_utc_day(l, kJul31Midnight, out, 4));
  EXPECT_EQ(1, events_on_utc_day(l, kJul31Midnight + 86400, out, 4));
  // Borne haute exclusive : le 2 août ne contient plus rien.
  EXPECT_EQ(0, events_on_utc_day(l, kJul31Midnight + 2 * 86400, out, 4));
  // Et le jour d'avant non plus.
  EXPECT_EQ(0, events_on_utc_day(l, kJul30Midnight - 86400, out, 4));
}

TEST(GcalRange, ShowsAnAllDayEventOnEveryDayItSpans) {
  // Google/HA donnent une fin EXCLUSIVE pour les journées entières :
  // 30 -> 02 = les 30 et 31 juillet + le 1er août.
  const gcal::EventList l =
      parse("[" + all_day_event("Vacances", "2026-07-30", "2026-08-02") + "]");
  const gcal::Event *out[4] = {};
  EXPECT_EQ(1, events_on_utc_day(l, kJul30Midnight, out, 4));
  EXPECT_EQ(1, events_on_utc_day(l, kJul31Midnight, out, 4));
  EXPECT_EQ(1, events_on_utc_day(l, kJul31Midnight + 86400, out, 4));
  EXPECT_EQ(0, events_on_utc_day(l, kJul31Midnight + 2 * 86400, out, 4));
}

TEST(GcalRange, AnAllDayEventDoesNotBleedIntoTheNextLocalDay) {
  // Le second bug : les journées entières sont ancrées à minuit UTC par le
  // parseur, la fenêtre de l'UI est en heure LOCALE. Sur un fuseau +02:00, un
  // all-day d'un seul jour finit à 02:00 locale le lendemain et apparaissait
  // aussi sur le jour suivant.
  const gcal::EventList l =
      parse("[" + all_day_event("Ferie", "2026-07-30", "2026-07-31") + "]");
  constexpr int64_t kOffset = 2 * 3600; // Europe/Paris en été
  const int64_t local_30 = kJul30Midnight - kOffset;
  const int64_t local_31 = kJul31Midnight - kOffset;
  const gcal::Event *out[4] = {};
  EXPECT_EQ(1, gcal::events_on_day(l, local_30, local_31, kJul30Midnight, out,
                                   4));
  EXPECT_EQ(0, gcal::events_on_day(l, local_31, local_31 + 86400,
                                   kJul31Midnight, out, 4));
}

TEST(GcalRange, KeepsATimedEventWithoutAnEndOnItsOwnDay) {
  // "end" absent -> end == start (cf. FallsBackToStartWhenEndIsMissing) : un
  // chevauchement strict rejetterait un événement de durée nulle tombant pile
  // à minuit.
  const gcal::EventList l =
      parse("[{\"summary\":\"Sans fin\",\"start\":{\"dateTime\":"
            "\"2026-07-30T00:00:00Z\"}}]");
  const gcal::Event *out[4] = {};
  EXPECT_EQ(1, events_on_utc_day(l, kJul30Midnight, out, 4));
  EXPECT_EQ(0, events_on_utc_day(l, kJul31Midnight, out, 4));
}

TEST(GcalRange, FlagsOnlyTheContinuationDays) {
  const gcal::EventList l =
      parse("[" +
            timed_event("Voyage", "2026-07-30T18:00:00Z",
                        "2026-08-01T09:00:00Z") +
            "," + all_day_event("Vacances", "2026-07-30", "2026-08-01") + "]");
  ASSERT_EQ(2, l.count);
  for (int i = 0; i < 2; ++i) {
    const gcal::Event &e = l.events[i];
    EXPECT_FALSE(gcal::starts_before_day(e, kJul30Midnight, kJul30Midnight))
        << e.summary;
    EXPECT_TRUE(gcal::starts_before_day(e, kJul31Midnight, kJul31Midnight))
        << e.summary;
  }
}

TEST(GcalRange, UtcMidnightMatchesTheHandComputedEpochs) {
  // Ancre indépendante du parseur (même valeur que kJul30Midnight, calculée à
  // la main en tête de fichier).
  EXPECT_EQ(kJul30Midnight, gcal::utc_midnight(2026, 7, 30));
  EXPECT_EQ(1'767'225'600, gcal::utc_midnight(2026, 1, 1));
  EXPECT_EQ(1'577'836'800, gcal::utc_midnight(2020, 1, 1));
}

// --- Fraîcheur ET couverture ------------------------------------------------

constexpr int64_t kMaxAgeUs = 15 * 60 * 1000000LL;

gcal::EventList cached_for(int64_t start_s, int64_t end_s, int64_t fetched_us) {
  gcal::EventList l;
  l.valid = true;
  l.range_start_s = start_s;
  l.range_end_s = end_s;
  l.fetched_at_us = fetched_us;
  return l;
}

TEST(GcalRange, CoversAFreshExactMatch) {
  const gcal::EventList l =
      cached_for(kJul30Midnight, kJul31Midnight, 1'000'000);
  EXPECT_TRUE(gcal::range_covers(l, kJul30Midnight, kJul31Midnight, 2'000'000,
                                 kMaxAgeUs));
}

TEST(GcalRange, CoversAWindowStrictlyInsideTheCachedRange) {
  // Cache d'une semaine entière : la fenêtre du jour est dedans, pas besoin de
  // refetch.
  const gcal::EventList l = cached_for(kJul30Midnight - 3 * 86400,
                                       kJul30Midnight + 4 * 86400, 1'000'000);
  EXPECT_TRUE(gcal::range_covers(l, kJul30Midnight, kJul31Midnight, 2'000'000,
                                 kMaxAgeUs));
}

TEST(GcalRange, DoesNotCoverAnotherPeriodEvenWhenFresh) {
  // Le cœur du bug : fraîcheur seule ne suffit pas. Le cache vient d'être
  // rempli par l'Agenda sur une autre semaine — il est frais ET inutilisable.
  const int64_t other = kJul30Midnight + 7 * 86400;
  const gcal::EventList l = cached_for(other, other + 7 * 86400, 1'000'000);
  EXPECT_FALSE(gcal::range_covers(l, kJul30Midnight, kJul31Midnight, 1'000'001,
                                  kMaxAgeUs));
}

TEST(GcalRange, DoesNotCoverAPartialOverlap) {
  // Cache couvrant la moitié de la fenêtre : refetch, sinon la fin de journée
  // manque silencieusement.
  const gcal::EventList l =
      cached_for(kJul30Midnight, kJul30Midnight + 43200, 1'000'000);
  EXPECT_FALSE(gcal::range_covers(l, kJul30Midnight, kJul31Midnight, 2'000'000,
                                  kMaxAgeUs));
}

TEST(GcalRange, DoesNotCoverAStaleRange) {
  const gcal::EventList l =
      cached_for(kJul30Midnight, kJul31Midnight, 1'000'000);
  EXPECT_FALSE(gcal::range_covers(l, kJul30Midnight, kJul31Midnight,
                                  1'000'000 + kMaxAgeUs + 1, kMaxAgeUs));
}

TEST(GcalRange, NeverCoversAnInvalidCache) {
  // valid=false = aucun fetch réussi depuis le boot ; les bornes valent 0 et
  // engloberaient n'importe quoi si on ne testait que les inégalités.
  gcal::EventList l = cached_for(0, 0, 0);
  l.valid = false;
  EXPECT_FALSE(gcal::range_covers(l, 0, 0, 0, kMaxAgeUs));
}

} // namespace
