#include <gtest/gtest.h>

#include "net/file_path.h"

#include <cstring>
#include <string>

namespace {

// Aide de lecture : renvoie le chemin assaini, ou "" si refusé.
std::string San(const char *raw, size_t cap = 128) {
  char out[256] = {0};
  if (cap > sizeof(out)) {
    cap = sizeof(out);
  }
  if (!file_path::sanitize(raw, out, cap)) {
    return "";
  }
  return std::string(out);
}

} // namespace

TEST(FilePath, NominalPasse) {
  EXPECT_EQ(San("/sdcard/log/2026-08-15.txt"), "/sdcard/log/2026-08-15.txt");
}

TEST(FilePath, RacineSeulePasse) {
  EXPECT_EQ(San("/sdcard"), "/sdcard");
  EXPECT_EQ(San("/sdcard/"), "/sdcard/");
}

TEST(FilePath, PourcentDecode) {
  EXPECT_EQ(San("/sdcard/mon%20fichier.wav"), "/sdcard/mon fichier.wav");
  EXPECT_EQ(San("/sdcard/a%2Fb"), "/sdcard/a/b");
}

TEST(FilePath, RemonteeRefusee) {
  EXPECT_EQ(San("/sdcard/../etc/passwd"), "");
  EXPECT_EQ(San("/sdcard/log/../../x"), "");
  EXPECT_EQ(San("/sdcard/.."), "");
  EXPECT_EQ(San("/sdcard/%2e%2e/x"), "");
}

TEST(FilePath, DoubleEncodageNeDevientPasUnPoint) {
  // "%252e%252e" -> "%2e%2e" : du texte, pas une remontée. Le chemin est donc
  // accepté, et ce qu'il désigne n'existe pas — c'est le comportement voulu.
  EXPECT_EQ(San("/sdcard/%252e%252e"), "/sdcard/%2e%2e");
}

TEST(FilePath, PointSimpleEtSeparateursRepetesPassent) {
  EXPECT_EQ(San("/sdcard/./log"), "/sdcard/./log");
  EXPECT_EQ(San("/sdcard//log"), "/sdcard//log");
}

TEST(FilePath, HorsRacineRefuse) {
  EXPECT_EQ(San("/etc/passwd"), "");
  EXPECT_EQ(San("log/x.txt"), "");
  EXPECT_EQ(San(""), "");
}

TEST(FilePath, PrefixeQuiRessembleRefuse) {
  // Le piège classique d'un memcmp de préfixe sans vérifier le séparateur.
  EXPECT_EQ(San("/sdcardXYZ/f"), "");
  EXPECT_EQ(San("/sdcard_backup/f"), "");
}

TEST(FilePath, EchappementInvalideRefuse) {
  EXPECT_EQ(San("/sdcard/a%"), "");
  EXPECT_EQ(San("/sdcard/a%2"), "");
  EXPECT_EQ(San("/sdcard/a%zz"), "");
}

TEST(FilePath, CaracteresInterditsRefuses) {
  EXPECT_EQ(San("/sdcard/a%00b"), ""); // NUL injecté
  EXPECT_EQ(San("/sdcard/a%0Ab"), ""); // saut de ligne
  EXPECT_EQ(San("/sdcard/a\\b"), "");  // antislash
}

TEST(FilePath, DepassementDeCapaciteRefuse) {
  // 20 octets de capacité pour un chemin plus long : refus, pas de troncature.
  char out[20];
  EXPECT_FALSE(file_path::sanitize("/sdcard/un_nom_de_fichier_long.txt", out,
                                   sizeof(out)));
}

TEST(FilePath, ArgumentsNulsRefuses) {
  char out[32];
  EXPECT_FALSE(file_path::sanitize(nullptr, out, sizeof(out)));
  EXPECT_FALSE(file_path::sanitize("/sdcard/x", nullptr, 32));
  EXPECT_FALSE(file_path::sanitize("/sdcard/x", out, 0));
}
