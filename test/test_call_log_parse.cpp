#include "app/call_log_parse.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

// Décodage du journal d'appels (/sdcard/calls.log). Les lignes de référence
// sont écrites à la main, au format que call_history::append_log produit — pas
// relevées sur une sortie du firmware, qui validerait aussi ses bugs.

namespace {

using call_log::Entry;

// Raccourci : parse une ligne C++ et renvoie le verdict + l'entrée décodée.
bool parse(const std::string &line, Entry &out) {
  return call_log::parse_line(line.c_str(), line.size(), out);
}

// --- parse_line --------------------------------------------------------------

TEST(CallLogParse, DecodesAMissedCallWithItsSnapshot) {
  Entry e{};
  ASSERT_TRUE(parse("2026-08-01 19:04:33,MANQUE,/sdcard/calls/call_007.jpg", e));
  EXPECT_STREQ("2026-08-01 19:04:33", e.stamp);
  EXPECT_STREQ("/sdcard/calls/call_007.jpg", e.path);
  EXPECT_TRUE(e.missed);
}

TEST(CallLogParse, DecodesAnAnsweredCallWithAnEmptyPath) {
  // Un appel répondu n'archive rien : le troisième champ est vide, la ligne
  // reste valide. C'est le cas le plus fréquent, il ne doit pas être rejeté.
  Entry e{};
  ASSERT_TRUE(parse("2026-08-01 19:10:00,REPONDU,", e));
  EXPECT_STREQ("2026-08-01 19:10:00", e.stamp);
  EXPECT_STREQ("", e.path);
  EXPECT_FALSE(e.missed);
}

TEST(CallLogParse, DecodesTheUnknownClockPlaceholder) {
  // Sans synchro SNTP, format_now écrit des zéros. La ligne doit rester
  // exploitable : l'appel a bien eu lieu, seule l'heure manque.
  Entry e{};
  ASSERT_TRUE(parse("0000-00-00 00:00:00,MANQUE,/sdcard/calls/call_000.jpg", e));
  EXPECT_STREQ("0000-00-00 00:00:00", e.stamp);
  EXPECT_TRUE(e.missed);
}

TEST(CallLogParse, TolerateTrailingEndOfLine) {
  // Le fichier peut transiter par le PC en mode clé USB, qui y laisse des CRLF.
  Entry e{};
  ASSERT_TRUE(parse("2026-08-01 19:04:33,MANQUE,/a.jpg\r\n", e));
  EXPECT_STREQ("/a.jpg", e.path);
}

TEST(CallLogParse, RejectsLinesWithTooFewFields) {
  Entry e{};
  EXPECT_FALSE(parse("", e));
  EXPECT_FALSE(parse("2026-08-01 19:04:33", e));
  EXPECT_FALSE(parse("2026-08-01 19:04:33,MANQUE", e));
}

TEST(CallLogParse, RejectsAnUnknownVerdict) {
  // Une coupure de courant en plein fwrite peut laisser n'importe quoi : mieux
  // vaut sauter la ligne que d'inventer un appel.
  Entry e{};
  EXPECT_FALSE(parse("2026-08-01 19:04:33,PEUTETRE,/a.jpg", e));
  EXPECT_FALSE(parse("2026-08-01 19:04:33,MANQU,/a.jpg", e));
  EXPECT_FALSE(parse("2026-08-01 19:04:33,MANQUEE,/a.jpg", e));
}

TEST(CallLogParse, RejectsAnEmptyTimestamp) {
  Entry e{};
  EXPECT_FALSE(parse(",MANQUE,/a.jpg", e));
}

TEST(CallLogParse, RejectsFieldsThatWouldNotFitTheEntry) {
  // La borne protège les memcpy de parse_line : une ligne trop longue doit être
  // refusée, jamais tronquée en silence.
  Entry e{};
  const std::string long_stamp(call_log::kStampMax, 'x');
  EXPECT_FALSE(parse(long_stamp + ",MANQUE,/a.jpg", e));
  const std::string long_path(call_log::kPathMax, 'p');
  EXPECT_FALSE(parse("2026-08-01 19:04:33,MANQUE," + long_path, e));

  // ...et la plus longue qui tienne, elle, passe.
  const std::string max_path(call_log::kPathMax - 1, 'p');
  ASSERT_TRUE(parse("2026-08-01 19:04:33,MANQUE," + max_path, e));
  EXPECT_EQ(max_path, std::string(e.path));
}

TEST(CallLogParse, DoesNotReadBeyondTheGivenLength) {
  // parse_line reçoit une longueur, pas une chaîne terminée : le reste du
  // tampon (la ligne suivante du journal) ne doit pas fuiter dans le chemin.
  const char block[] = "2026-08-01 19:04:33,MANQUE,/a.jpg\n2026-08-02 08:00:00,REPONDU,";
  Entry e{};
  ASSERT_TRUE(call_log::parse_line(block, 33, e));
  EXPECT_STREQ("/a.jpg", e.path);
}

// --- parse_tail --------------------------------------------------------------

// Journal de référence : 4 lignes valides, la plus ancienne en premier (l'ordre
// dans lequel append_log les écrit).
constexpr char kLog[] =
    "2026-08-01 08:00:00,REPONDU,\n"
    "2026-08-01 12:30:15,MANQUE,/sdcard/calls/call_001.jpg\n"
    "2026-08-01 18:45:02,MANQUE,/sdcard/calls/call_002.jpg\n"
    "2026-08-02 07:10:44,REPONDU,\n";

size_t tail(const char *text, bool partial, Entry *out, size_t max) {
  return call_log::parse_tail(text, strlen(text), partial, out, max);
}

TEST(CallLogTail, ReturnsMostRecentFirst) {
  // C'est l'ordre d'affichage de la page Appels : le journal est chronologique,
  // la liste ne l'est pas.
  Entry e[8]{};
  ASSERT_EQ(4u, tail(kLog, false, e, 8));
  EXPECT_STREQ("2026-08-02 07:10:44", e[0].stamp);
  EXPECT_STREQ("2026-08-01 18:45:02", e[1].stamp);
  EXPECT_STREQ("2026-08-01 12:30:15", e[2].stamp);
  EXPECT_STREQ("2026-08-01 08:00:00", e[3].stamp);
  EXPECT_STREQ("/sdcard/calls/call_002.jpg", e[1].path);
}

TEST(CallLogTail, KeepsTheNewestWhenTheCapIsReached) {
  // Le cap doit couper les VIEILLES entrées, pas les récentes.
  Entry e[2]{};
  ASSERT_EQ(2u, tail(kLog, false, e, 2));
  EXPECT_STREQ("2026-08-02 07:10:44", e[0].stamp);
  EXPECT_STREQ("2026-08-01 18:45:02", e[1].stamp);
}

TEST(CallLogTail, SkipsTheFirstLineWhenTheBlockIsAPartialRead) {
  // load_recent ne relit que la queue du fichier : la première ligne du bloc
  // est presque toujours coupée en deux. La garder produirait un horodatage
  // amputé en tête de liste.
  Entry e[8]{};
  ASSERT_EQ(3u, tail(kLog, true, e, 8));
  EXPECT_STREQ("2026-08-02 07:10:44", e[0].stamp);
  EXPECT_STREQ("2026-08-01 12:30:15", e[2].stamp);
}

TEST(CallLogTail, PartialFlagAppliesEvenWhenTheCapAlsoBites) {
  // Les deux mécanismes de rejet se cumulent : le partiel enlève la ligne 1,
  // le cap enlève ensuite la plus ancienne de ce qui reste.
  Entry e[2]{};
  ASSERT_EQ(2u, tail(kLog, true, e, 2));
  EXPECT_STREQ("2026-08-02 07:10:44", e[0].stamp);
  EXPECT_STREQ("2026-08-01 18:45:02", e[1].stamp);
}

TEST(CallLogTail, DropsCorruptedLinesWithoutLosingTheGoodOnes) {
  constexpr char kMixed[] = "2026-08-01 08:00:00,REPONDU,\n"
                            "ligne corrompue sans virgule\n"
                            "\n"
                            "2026-08-01 12:30:15,MANQUE,/a.jpg\n";
  Entry e[8]{};
  ASSERT_EQ(2u, tail(kMixed, false, e, 8));
  EXPECT_STREQ("2026-08-01 12:30:15", e[0].stamp);
  EXPECT_STREQ("2026-08-01 08:00:00", e[1].stamp);
}

TEST(CallLogTail, AcceptsAFinalLineWithoutNewline) {
  // Une écriture interrompue avant le '\n' final ne doit pas faire perdre le
  // dernier appel, qui est justement le plus intéressant.
  constexpr char kNoEol[] = "2026-08-01 08:00:00,REPONDU,\n"
                            "2026-08-01 12:30:15,MANQUE,/a.jpg";
  Entry e[8]{};
  ASSERT_EQ(2u, tail(kNoEol, false, e, 8));
  EXPECT_STREQ("2026-08-01 12:30:15", e[0].stamp);
}

TEST(CallLogTail, HandlesEmptyAndDegenerateInputs) {
  Entry e[4]{};
  EXPECT_EQ(0u, call_log::parse_tail("", 0, false, e, 4));
  EXPECT_EQ(0u, call_log::parse_tail(kLog, strlen(kLog), false, nullptr, 4));
  EXPECT_EQ(0u, call_log::parse_tail(kLog, strlen(kLog), false, e, 0));
  EXPECT_EQ(0u, call_log::parse_tail(nullptr, 10, false, e, 4));
  // Un bloc d'UNE seule ligne, déclaré partiel, ne contient rien d'exploitable.
  EXPECT_EQ(0u, tail("2026-08-01 08:00:00,REPONDU,\n", true, e, 4));
}

TEST(CallLogTail, ASingleEntryIsNotReversedIntoOblivion) {
  // Le retournement en place a un cas limite à n == 1 : la boucle ne doit pas
  // s'exécuter, sous peine d'échanger l'entrée avec elle-même hors bornes.
  Entry e[4]{};
  ASSERT_EQ(1u, tail("2026-08-01 08:00:00,MANQUE,/a.jpg\n", false, e, 4));
  EXPECT_STREQ("2026-08-01 08:00:00", e[0].stamp);
  EXPECT_STREQ("/a.jpg", e[0].path);
}

} // namespace
