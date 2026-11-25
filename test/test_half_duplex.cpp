#include <gtest/gtest.h>

#include "media/half_duplex.h"

#include <cstdint>
#include <vector>

// Vecteurs calculés à la main à partir des constantes du header :
// kAttackStep = (32768 - 3277) / 40 = 737, kReleaseStep = 29491 / 800 = 36.

namespace {

std::vector<int16_t> constant(size_t n, int16_t v) {
  return std::vector<int16_t>(n, v);
}

} // namespace

TEST(HalfDuplexPeak, AbsoluteMaximum) {
  const int16_t pcm[] = {0, -5, 3};
  EXPECT_EQ(half_duplex::peak(pcm, 3), 5);
}

TEST(HalfDuplexPeak, MostNegativeSampleClampsTo32767) {
  const int16_t pcm[] = {100, -32768};
  EXPECT_EQ(half_duplex::peak(pcm, 2), 32767);
}

TEST(HalfDuplexPeak, EmptyFrameIsZero) {
  EXPECT_EQ(half_duplex::peak(nullptr, 0), 0);
}

TEST(HalfDuplexBefore, SurvivesCounterWrap) {
  EXPECT_TRUE(half_duplex::before(0xFFFFFFF0u, 0x10u));
  EXPECT_FALSE(half_duplex::before(0x20u, 0x10u));
  EXPECT_FALSE(half_duplex::before(0x10u, 0x10u));
}

TEST(HalfDuplexFar, LoudFrameOpensWindowForBlockPlusHold) {
  half_duplex::clear_far();
  const auto loud = constant(160, 1000); // 160 éch. = 20 ms, crête = seuil
  half_duplex::note_far(loud.data(), loud.size(), 1000);
  // Fenêtre jusqu'à 1000 + 20 + 300 = 1320 ms, exclu.
  EXPECT_TRUE(half_duplex::far_speaking(1000));
  EXPECT_TRUE(half_duplex::far_speaking(1319));
  EXPECT_FALSE(half_duplex::far_speaking(1320));
}

TEST(HalfDuplexFar, QuietFrameDoesNotOpenOrExtendWindow) {
  half_duplex::clear_far();
  const auto quiet = constant(160, 999);
  half_duplex::note_far(quiet.data(), quiet.size(), 1000);
  EXPECT_FALSE(half_duplex::far_speaking(1000));

  const auto loud = constant(160, -2000);
  half_duplex::note_far(loud.data(), loud.size(), 1000); // jusqu'à 1320
  half_duplex::note_far(quiet.data(), quiet.size(), 1300);
  EXPECT_FALSE(half_duplex::far_speaking(1320));
}

TEST(HalfDuplexFar, NothingSeenMeansNotSpeakingEvenAtTimeZero) {
  half_duplex::clear_far();
  EXPECT_FALSE(half_duplex::far_speaking(0));
  EXPECT_FALSE(half_duplex::far_speaking(0xFFFFFF00u));
}

TEST(HalfDuplexDucker, AttackFirstSampleThenFloor) {
  half_duplex::Ducker d;
  auto pcm = constant(100, 10000);
  d.apply(pcm.data(), pcm.size(), true);
  // 1er échantillon : gain 32768 - 737 = 32031 ; 10000 * 32031 >> 15 = 9775.
  EXPECT_EQ(pcm[0], 9775);
  // Plancher atteint au 41e : 10000 * 3277 >> 15 = 1000.
  EXPECT_EQ(pcm[99], 1000);
  EXPECT_EQ(d.gain_q15(), 3277);
}

TEST(HalfDuplexDucker, ReleaseRampsBackToUnity) {
  half_duplex::Ducker d;
  auto pcm = constant(100, 10000);
  d.apply(pcm.data(), pcm.size(), true);

  auto rel = constant(1000, 10000);
  d.apply(rel.data(), rel.size(), false);
  // 1er échantillon : gain 3277 + 36 = 3313 ; 10000 * 3313 >> 15 = 1011.
  EXPECT_EQ(rel[0], 1011);
  // 29491 / 36 -> unité atteinte avant le 820e échantillon.
  EXPECT_EQ(rel[999], 10000);
  EXPECT_EQ(d.gain_q15(), 32768);
}

TEST(HalfDuplexDucker, UnityLeavesSignalUntouched) {
  half_duplex::Ducker d;
  int16_t pcm[] = {-32768, -1, 0, 1, 32767};
  d.apply(pcm, 5, false);
  EXPECT_EQ(pcm[0], -32768);
  EXPECT_EQ(pcm[1], -1);
  EXPECT_EQ(pcm[2], 0);
  EXPECT_EQ(pcm[3], 1);
  EXPECT_EQ(pcm[4], 32767);
}

TEST(HalfDuplexDucker, ResetRestoresUnity) {
  half_duplex::Ducker d;
  auto pcm = constant(100, 10000);
  d.apply(pcm.data(), pcm.size(), true);
  d.reset();
  EXPECT_EQ(d.gain_q15(), 32768);
}
