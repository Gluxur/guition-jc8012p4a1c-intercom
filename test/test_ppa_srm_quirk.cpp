// Tests du contournement du bug matériel PPA DIG-734 (cf. ppa_srm_quirk.h).
//
// Les valeurs attendues sont dérivées À LA MAIN de la formule du driver
// (ESP-IDF 5.4.4, components/esp_driver_ppa/src/ppa_srm.c:159-179) et de la
// géométrie réelle après rotation — jamais en exécutant l'implémentation
// testée, qui validerait aussi ses propres erreurs.
//
// Rappel de la formule, pour un flush LVGL (RGB565 -> RGB565, échelle 1:1) :
//   unité de sortie = 64 px de large (128 octets) x 16 lignes (macro-bloc SRM)
//   dernier bloc = (W % 64 ou 64) x (H % 16 ou 16) pixels
//   il doit valoir >= 1536 bits (FIFO 2D-DMA), soit >= 96 pixels en RGB565
// Le driver applique cette formule à (w, h) — la bande AVANT rotation — alors
// que la sortie réelle en 270° fait h de large et w de haut.

#include <gtest/gtest.h>

#include "ppa_srm_quirk.h"

using bsp::ppa_quirk::kMaxSplitParts;
using bsp::ppa_quirk::rot270_band_stalls;
using bsp::ppa_quirk::rot270_split;

namespace {

struct Split {
  std::uint32_t parts = 0;
  std::uint32_t heights[kMaxSplitParts] = {};

  std::uint32_t total() const {
    std::uint32_t sum = 0;
    for (std::uint32_t i = 0; i < parts; ++i) {
      sum += heights[i];
    }
    return sum;
  }
};

Split split_of(std::uint32_t w, std::uint32_t h) {
  Split s;
  s.parts = rot270_split(w, h, s.heights, kMaxSplitParts);
  return s;
}

} // namespace

// La bande observée sur cible : bouton « Semaine/Mois » de la page Agenda
// (220x64 + bordure 1 px => aire invalidée 224x68).
//   sortie réelle  : 68 large x 224 haut -> dernier bloc (68%64)=4 x (224%16)=16
//                    -> 64 px < 96  => le contournement est NÉCESSAIRE
//   calcul driver  : 224 large x 68 haut -> (224%64)=32 x (68%16)=4
//                    -> 128 px >= 96 => le driver ne l'applique PAS
// => transaction sans fin, moteur SRM mort pour la session.
TEST(PpaSrmQuirk, ObservedAgendaBandStalls) {
  EXPECT_TRUE(rot270_band_stalls(224, 68));
}

TEST(PpaSrmQuirk, ObservedAgendaBandSplitsIntoSafeHalves) {
  const Split s = split_of(224, 68);
  ASSERT_EQ(s.parts, 2u);
  EXPECT_EQ(s.total(), 68u);
  EXPECT_FALSE(rot270_band_stalls(224, s.heights[0]));
  EXPECT_FALSE(rot270_band_stalls(224, s.heights[1]));
}

// Bandes vues en permanence sur les autres pages, qui n'ont jamais figé
// l'appareil : le contournement ne doit pas les découper.
TEST(PpaSrmQuirk, EverydayBandsAreLeftAlone) {
  // Repaint pleine largeur : 81920 o de draw buffer / (1280 px * 2 o) = 32 l.
  //   réel : 32 large x 1280 haut -> 32 x 16 = 512 >= 96
  EXPECT_FALSE(rot270_band_stalls(1280, 32));
  const Split s = split_of(1280, 32);
  EXPECT_EQ(s.parts, 1u); // une seule transaction, comme avant
  EXPECT_EQ(s.heights[0], 32u);
  // Étiquette d'horloge ~150x40 -> réel 40 x (150%16=6) = 240 >= 96
  EXPECT_FALSE(rot270_band_stalls(150, 40));
  // Barre de navigation 480x72 -> réel (72%64)=8 x (480%16)=16 = 128 >= 96
  EXPECT_FALSE(rot270_band_stalls(480, 72));
  // Bande minuscule : ni plus large qu'une unité, ni plus haute qu'un
  // macro-bloc -> le découpage en unités n'a pas lieu, aucun bloc résiduel.
  EXPECT_FALSE(rot270_band_stalls(16, 64));
}

// Le driver applique déjà le contournement (sa formule voit un petit bloc) :
// rien à faire de notre côté, même si la géométrie tournée en voit un aussi.
//   300x50 -> driver : (300%64)=44 x (50%16)=2 = 88 < 96 => bypass activé
TEST(PpaSrmQuirk, DriverAlreadyBypassesLeavesBandAlone) {
  EXPECT_FALSE(rot270_band_stalls(300, 50));
}

TEST(PpaSrmQuirk, DegenerateBands) {
  EXPECT_FALSE(rot270_band_stalls(0, 0));
  EXPECT_FALSE(rot270_band_stalls(224, 0));
  EXPECT_FALSE(rot270_band_stalls(0, 68));
  std::uint32_t heights[kMaxSplitParts] = {};
  EXPECT_EQ(rot270_split(0, 68, heights, kMaxSplitParts), 0u);
  EXPECT_EQ(rot270_split(224, 68, heights, 0), 0u);
  EXPECT_EQ(rot270_split(224, 68, nullptr, kMaxSplitParts), 0u);
}

// Propriété centrale, sur tout le domaine des bandes que LVGL peut émettre ici
// (largeur logique <= 1280, hauteur bornée par le draw buffer de 81920 o) :
// une découpe couvre exactement la bande et aucune de ses parts ne bloque.
TEST(PpaSrmQuirk, SplitCoversBandAndYieldsOnlySafeParts) {
  for (uint32_t w = 1; w <= 1280; ++w) {
    const uint32_t h_max = 81920u / (w * 2u);
    for (uint32_t h = 1; h <= h_max; ++h) {
      const Split s = split_of(w, h);
      if (s.parts == 0) {
        continue;
      }
      ASSERT_EQ(s.total(), h) << "bande " << w << "x" << h;
      for (uint32_t i = 0; i < s.parts; ++i) {
        ASSERT_GT(s.heights[i], 0u) << "bande " << w << "x" << h;
        ASSERT_FALSE(rot270_band_stalls(w, s.heights[i]))
            << "bande " << w << "x" << h << " part " << i;
      }
    }
  }
}

// Résidu assumé : certaines bandes n'ont AUCUNE découpe en hauteur sûre (leur
// plus petite sous-bande sûre dépasse la moitié de leur hauteur). Le flush les
// soumet telles quelles, avec un avertissement. Ce test borne le résidu pour
// qu'une régression du découpage se voie ; il vaut aussi documentation : le
// cas ne concerne pas les largeurs des bandes réellement émises ici.
TEST(PpaSrmQuirk, UnsplittableBandsStayMarginal) {
  uint32_t total = 0;
  uint32_t unsplittable = 0;
  for (uint32_t w = 1; w <= 1280; ++w) {
    const uint32_t h_max = 81920u / (w * 2u);
    for (uint32_t h = 1; h <= h_max; ++h) {
      ++total;
      if (split_of(w, h).parts == 0) {
        ++unsplittable;
      }
    }
  }
  EXPECT_LE(unsplittable * 50, total)
      << unsplittable << " bandes sans découpe sur " << total;
}

// Les largeurs des widgets réellement dessinés ici n'ont qu'une poignée de
// hauteurs dangereuses, toutes découpables — c'est ce qui explique que
// l'appareil ait fonctionné des mois avant que le bouton Semaine/Mois de
// l'Agenda ne tombe pile sur l'une d'elles (224x68).
TEST(PpaSrmQuirk, RealWidgetWidthsAreAlwaysSplittable) {
  for (uint32_t w : {224u, 480u, 800u, 1280u, 220u, 68u}) {
    uint32_t dangerous = 0;
    for (uint32_t h = 1; h <= 81920u / (w * 2u); ++h) {
      if (rot270_band_stalls(w, h)) {
        ++dangerous;
        ASSERT_NE(split_of(w, h).parts, 0u) << "bande " << w << "x" << h;
      }
    }
    EXPECT_LT(dangerous, 16u) << "largeur " << w;
  }
}
