#include "media/g711.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <set>

namespace {

// g711 garde sa table de décodage dans un état global rempli par init().
// La fixture le réinitialise avant chaque test pour qu'aucun test ne dépende
// de l'ordre d'exécution.
class G711 : public ::testing::Test {
protected:
  void SetUp() override { g711::init(); }
};

// --- Vecteurs connus --------------------------------------------------------
// Valeurs calculées à la main depuis la définition A-law de la RFC 3551, PAS
// obtenues en exécutant le code : un vecteur régénéré par l'implémentation
// testée validerait aussi ses bugs.

TEST_F(G711, EncodesKnownVectors) {
  EXPECT_EQ(0xD5, g711::pcm16_to_alaw(0));
  EXPECT_EQ(0x55, g711::pcm16_to_alaw(-8));
  EXPECT_EQ(0xAA, g711::pcm16_to_alaw(32767));
  EXPECT_EQ(0x2A, g711::pcm16_to_alaw(-32768));
}

TEST_F(G711, DecodesKnownVectors) {
  EXPECT_EQ(8, g711::alaw_to_pcm16(0xD5));
  EXPECT_EQ(-8, g711::alaw_to_pcm16(0x55));
  EXPECT_EQ(32256, g711::alaw_to_pcm16(0xAA));
  EXPECT_EQ(-32256, g711::alaw_to_pcm16(0x2A));
}

// --- Propriétés sur tout le domaine ----------------------------------------
// L'encodeur (algorithme ITU) et le décodeur (table) sont deux implémentations
// indépendantes : c'est leur désynchronisation qui casserait l'audio, et elle
// ne se voit que sur une couverture exhaustive.

TEST_F(G711, RoundTripStaysWithinQuantisationError) {
  for (int32_t sample = std::numeric_limits<int16_t>::min();
       sample <= std::numeric_limits<int16_t>::max(); ++sample) {
    const int16_t original = static_cast<int16_t>(sample);
    const int16_t decoded = g711::alaw_to_pcm16(g711::pcm16_to_alaw(original));
    const int32_t error = static_cast<int32_t>(decoded) - sample;

    // Borne A-law : erreur relative bornée (segments logarithmiques) plus un
    // terme absolu pour le segment linéaire autour de zéro.
    const int32_t tolerance = (sample < 0 ? -sample : sample) / 16 + 16;
    ASSERT_GE(error, -tolerance) << "sample=" << sample;
    ASSERT_LE(error, tolerance) << "sample=" << sample;
  }
}

TEST_F(G711, RoundTripPreservesSign) {
  for (int32_t sample = std::numeric_limits<int16_t>::min();
       sample <= std::numeric_limits<int16_t>::max(); ++sample) {
    const int16_t original = static_cast<int16_t>(sample);
    const int16_t decoded = g711::alaw_to_pcm16(g711::pcm16_to_alaw(original));

    // A-law n'a pas de zéro : ±0 se décode en ±8. Seul le signe doit survivre,
    // un basculement inverserait la phase du signal micro.
    if (sample >= 0) {
      ASSERT_GT(decoded, 0) << "sample=" << sample;
    } else {
      ASSERT_LT(decoded, 0) << "sample=" << sample;
    }
  }
}

TEST_F(G711, EveryCodeDecodesToADistinctLevel) {
  std::set<int16_t> levels;
  for (int code = 0; code < 256; ++code) {
    levels.insert(g711::alaw_to_pcm16(static_cast<uint8_t>(code)));
  }
  // 256 codes = 256 paliers (128 positifs, 128 négatifs). Un doublon
  // signalerait un recouvrement de segments dans le calcul de la table.
  EXPECT_EQ(256u, levels.size());
}

TEST_F(G711, DecodedLevelsNeverOverflowInt16) {
  for (int code = 0; code < 256; ++code) {
    const int16_t level = g711::alaw_to_pcm16(static_cast<uint8_t>(code));
    // Le magnitude max théorique est 32256 : un dépassement se replierait en
    // valeur de signe opposé (clic audible plein volume).
    EXPECT_LE(level, 32256) << "code=" << code;
    EXPECT_GE(level, -32256) << "code=" << code;
  }
}

// --- Régression -------------------------------------------------------------

TEST_F(G711, EncodesNegativesAsTheFoldedMirrorOfPositives) {
  // Les négatifs passent par `v = -v - 1` avant codage : le miroir exact de
  // l'échantillon `x` n'est donc PAS `-x` mais `-x - 8` (un pas de quantif du
  // segment linéaire). Les deux codes ne doivent alors différer que par le bit
  // de signe — les masques 0xD5 / 0x55 ne diffèrent que de 0x80.
  // Un décalage ici mettrait une composante continue sur le flux micro.
  for (int32_t magnitude = 0; magnitude <= 32760; magnitude += 8) {
    const int16_t positive = static_cast<int16_t>(magnitude);
    const int16_t mirrored = static_cast<int16_t>(-magnitude - 8);
    const uint8_t encoded_pos = g711::pcm16_to_alaw(positive);
    const uint8_t encoded_neg = g711::pcm16_to_alaw(mirrored);
    ASSERT_EQ(0x80, encoded_pos ^ encoded_neg) << "magnitude=" << magnitude;
  }
}

} // namespace
