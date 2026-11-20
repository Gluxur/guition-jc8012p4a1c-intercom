#include <gtest/gtest.h>

#include <cstring>

#include "app/ephemeris.h"

// Vecteurs relevés à la main sur un calendrier liturgique, JAMAIS produits en
// exécutant la table testée — un vecteur généré depuis l'implémentation
// validerait aussi ses erreurs d'indexation.

namespace {

// Longueur en CARACTÈRES affichés, pas en octets : les accents pèsent 2 octets
// en UTF-8 et fausseraient d'un tiers la mesure de largeur à l'écran. Compte
// les octets qui ne sont pas une continuation UTF-8 (10xxxxxx).
size_t glyph_len(const char *s) {
  size_t n = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
    if ((*p & 0xC0) != 0x80)
      ++n;
  return n;
}

} // namespace

TEST(Ephemeris, BornesAnnee) {
  EXPECT_STREQ("Ste Marie Mère de Dieu, St Fulgence",
               ephemeris::saint_of_day(0, 1));
  EXPECT_STREQ("St Sylvestre Ier, Ste Mélanie",
               ephemeris::saint_of_day(11, 31));
}

// Le décalage d'index est le seul vrai piège de ce module : ces trois-là
// tombent si kMonthStart part d'une disposition d'année commune (février à 28
// jours) au lieu de bissextile.
TEST(Ephemeris, FrontiereFevrierMars) {
  EXPECT_STREQ("St Auguste, St Oswald", ephemeris::saint_of_day(1, 29));
  EXPECT_STREQ("St Aubin, St David de Galles", ephemeris::saint_of_day(2, 1));
  EXPECT_STREQ("St Benjamin, Ste Balbine", ephemeris::saint_of_day(2, 31));
}

// Un jour par mois, échantillonné loin des bornes : attrape un cumul de
// longueurs de mois faux dès le premier mois concerné.
TEST(Ephemeris, UnJourParMois) {
  EXPECT_STREQ("St Marcel, St Honorat", ephemeris::saint_of_day(0, 16));
  EXPECT_STREQ("St Valentin, Sts Cyrille et Méthode",
               ephemeris::saint_of_day(1, 14));
  EXPECT_STREQ("St Joseph, époux de la Vierge", ephemeris::saint_of_day(2, 19));
  EXPECT_STREQ("St Marc évangéliste, St Anien", ephemeris::saint_of_day(3, 25));
  EXPECT_STREQ("St Yves, St Célestin V", ephemeris::saint_of_day(4, 19));
  EXPECT_STREQ("Nativité de St Jean-Baptiste", ephemeris::saint_of_day(5, 24));
  EXPECT_STREQ("St Camille de Lellis, St Donald",
               ephemeris::saint_of_day(6, 14));
  EXPECT_STREQ("Assomption de la Vierge, St Tarcisius",
               ephemeris::saint_of_day(7, 15));
  EXPECT_STREQ("St Matthieu apôtre, St Jonas", ephemeris::saint_of_day(8, 21));
  EXPECT_STREQ("St Denis de Paris, St Jean Leonardi",
               ephemeris::saint_of_day(9, 9));
  EXPECT_STREQ("Toussaint, St Bénigne", ephemeris::saint_of_day(10, 1));
  EXPECT_STREQ("Noël, Nativité du Seigneur", ephemeris::saint_of_day(11, 25));
}

// Régression : la première version ne gardait qu'un nom par jour et abrégeait
// les fêtes groupées, ce qui faisait disparaître des prénoms courants.
TEST(Ephemeris, SaintsSecondairesConserves) {
  EXPECT_NE(nullptr, strstr(ephemeris::saint_of_day(8, 29), "Gabriel"));
  EXPECT_NE(nullptr, strstr(ephemeris::saint_of_day(8, 29), "Raphaël"));
  EXPECT_NE(nullptr, strstr(ephemeris::saint_of_day(1, 27), "Gabriel"));
  EXPECT_NE(nullptr, strstr(ephemeris::saint_of_day(2, 22), "Léa"));
}

TEST(Ephemeris, HorsBornes) {
  EXPECT_STREQ("", ephemeris::saint_of_day(-1, 1));
  EXPECT_STREQ("", ephemeris::saint_of_day(12, 1));
  EXPECT_STREQ("", ephemeris::saint_of_day(0, 0));
  EXPECT_STREQ("", ephemeris::saint_of_day(0, 32));
  // 31 avril / 30 février n'existent pas, mais tm ne l'interdit pas : la borne
  // ne doit pas déborder sur le mois suivant.
  EXPECT_STREQ("", ephemeris::saint_of_day(3, 31));
  EXPECT_STREQ("", ephemeris::saint_of_day(1, 30));
}

// Toute la table est renseignée, et tient dans la largeur disponible : ~500 px
// à droite de la date en montserrat_fr_24, soit ~38 caractères. Au-delà, le
// libellé se coupe en "…" — pas une faute, mais un aveu qu'une entrée aurait dû
// être abrégée à la main plutôt que par LVGL.
TEST(Ephemeris, TableCompleteEtTenantEnLargeur) {
  static const int kLen[12] = {31, 29, 31, 30, 31, 30,
                               31, 31, 30, 31, 30, 31};
  for (int m = 0; m < 12; ++m)
    for (int d = 1; d <= kLen[m]; ++d) {
      const char *s = ephemeris::saint_of_day(m, d);
      const size_t n = glyph_len(s);
      EXPECT_GT(n, 0u) << "vide au " << d << "/" << (m + 1);
      EXPECT_LE(n, 38u) << "trop long au " << d << "/" << (m + 1) << " : " << s;
    }
}
