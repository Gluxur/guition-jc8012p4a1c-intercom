#include "app/motion_detect.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

constexpr int kW = 1288;
constexpr int kH = 728;
constexpr int kStride = kW * 10 / 8;

// Image RAW10 compactée dont chaque pixel vaut pix(x, y) en 8 bits de poids
// fort. Le 5e octet de chaque groupe (bits de poids faible) est mis à 0xFF :
// s'il était lu par erreur comme un pixel, toutes les moyennes bougeraient.
template <typename F> std::vector<uint8_t> make_frame(F pix) {
  std::vector<uint8_t> f(static_cast<size_t>(kStride) * kH);
  for (int y = 0; y < kH; y++) {
    uint8_t *row = f.data() + static_cast<size_t>(y) * kStride;
    for (int g = 0; g < kW / 4; g++) {
      for (int i = 0; i < 4; i++) {
        row[g * 5 + i] = pix(g * 4 + i, y);
      }
      row[g * 5 + 4] = 0xFF;
    }
  }
  return f;
}

motion::Grid flat(uint8_t v) {
  motion::Grid g{};
  for (auto &r : g.cell) {
    for (auto &c : r) {
      c = v;
    }
  }
  g.mean = v;
  return g;
}

} // namespace

TEST(MotionReduce, UniformFrameGivesUniformGrid) {
  const auto f = make_frame([](int, int) { return uint8_t{100}; });
  motion::Grid g{};
  motion::reduce_raw10(f.data(), kW, kH, 4, g);
  EXPECT_EQ(g.mean, 100);
  EXPECT_EQ(g.cell[0][0], 100);
  EXPECT_EQ(g.cell[motion::kGridH - 1][motion::kGridW - 1], 100);
}

// Frontière de cellule en x = 20 * 1288 / 40 = 644 : gauche à 200, droite à 50.
TEST(MotionReduce, HalvesLandInTheirCells) {
  const auto f =
      make_frame([](int x, int) { return x < 644 ? uint8_t{200} : uint8_t{50}; });
  motion::Grid g{};
  motion::reduce_raw10(f.data(), kW, kH, 4, g);
  EXPECT_EQ(g.cell[10][0], 200);
  EXPECT_EQ(g.cell[10][19], 200);
  EXPECT_EQ(g.cell[10][20], 50);
  EXPECT_EQ(g.cell[10][39], 50);
  EXPECT_EQ(g.mean, 125); // (20*200 + 20*50) / 40
}

// Frontière de ligne en y = 12 * 728 / 24 = 364.
TEST(MotionReduce, RowsLandInTheirCells) {
  const auto f =
      make_frame([](int, int y) { return y < 364 ? uint8_t{30} : uint8_t{90}; });
  motion::Grid g{};
  motion::reduce_raw10(f.data(), kW, kH, 4, g);
  EXPECT_EQ(g.cell[11][5], 30);
  EXPECT_EQ(g.cell[12][5], 90);
}

TEST(MotionCompare, IdenticalGridsDoNotMove) {
  const auto a = flat(80);
  const auto d = motion::compare(a, a, 10);
  EXPECT_EQ(d.changed, 0);
  EXPECT_EQ(d.max_delta, 0);
}

// Toute l'image +20 (nuage, pas d'exposition) : pas un mouvement.
TEST(MotionCompare, GlobalShiftIsIgnored) {
  const auto d = motion::compare(flat(80), flat(100), 10);
  EXPECT_EQ(d.changed, 0);
}

// 4 cellules +40 sur 960 : la moyenne entière ne bouge pas (160/960 = 0),
// donc l'écart retenu est exactement 40.
TEST(MotionCompare, LocalChangeIsCounted) {
  const auto a = flat(80);
  auto b = flat(80);
  b.cell[3][3] = b.cell[3][4] = b.cell[4][3] = b.cell[4][4] = 120;
  const auto d = motion::compare(a, b, 10);
  EXPECT_EQ(d.changed, 4);
  EXPECT_EQ(d.max_delta, 40);
}

// Deux cellules éloignées : le cadre englobe les deux, bornes incluses.
TEST(MotionCompare, BoxBoundsChangedCells) {
  const auto a = flat(80);
  auto b = flat(80);
  b.cell[5][3] = 120;  // ligne 5, colonne 3
  b.cell[2][10] = 120; // ligne 2, colonne 10
  const auto d = motion::compare(a, b, 10);
  EXPECT_EQ(d.x0, 3);
  EXPECT_EQ(d.y0, 2);
  EXPECT_EQ(d.x1, 10);
  EXPECT_EQ(d.y1, 5);
}

TEST(MotionCompare, NoChangeGivesNoBox) {
  const auto a = flat(80);
  const auto d = motion::compare(a, a, 10);
  EXPECT_EQ(d.x0, -1);
  EXPECT_EQ(d.y0, -1);
  EXPECT_EQ(d.x1, -1);
  EXPECT_EQ(d.y1, -1);
}

TEST(MotionCompare, ChangeAtThresholdIsNotCounted) {
  const auto a = flat(80);
  auto b = flat(80);
  b.cell[0][0] = 90;
  EXPECT_EQ(motion::compare(a, b, 10).changed, 0);
  b.cell[0][0] = 91;
  EXPECT_EQ(motion::compare(a, b, 10).changed, 1);
}

// Paramètres du capteur : noir 16, consigne 70, pose 4..6976 lignes, gain
// max x15,5 (248/16).
namespace {
constexpr uint8_t kBlack = 16;
constexpr uint8_t kTarget = 70;
constexpr uint32_t kMaxLines = 6976;
constexpr uint32_t kMinLines = 4;
constexpr uint32_t kMaxGain = 248;

motion::Exposure ae(motion::Exposure cur, uint8_t mean) {
  return motion::next_exposure(cur, mean, kTarget, kBlack, kMaxLines,
                               kMinLines, kMaxGain);
}
} // namespace

TEST(MotionAe, OnTargetIsUnchanged) {
  const auto e = ae({1000, 16}, 70);
  EXPECT_EQ(e.lines, 1000u);
  EXPECT_EQ(e.gain_x16, 16u);
}

// Signal 50 pour 54 voulus : rapport 1,08, dans la bande morte.
TEST(MotionAe, InsideDeadBandIsUnchanged) {
  const auto e = ae({1000, 16}, 66);
  EXPECT_EQ(e.lines, 1000u);
}

// Image au niveau du noir : pas maximal x2 sur la pose, gain inchangé.
TEST(MotionAe, BlackImageDoublesExposure) {
  const auto e = ae({1000, 16}, kBlack);
  EXPECT_EQ(e.lines, 2000u);
  EXPECT_EQ(e.gain_x16, 16u);
}

// Le piédestal compte : mesuré 43 = signal 27 pour 54 voulus, soit x2 exact.
// Sans le retrait du noir on lirait 70/43 = x1,63 -> 1625 lignes.
TEST(MotionAe, BlackLevelIsSubtracted) {
  const auto e = ae({1000, 16}, 43);
  EXPECT_EQ(e.lines, 2000u);
}

// Image saturée : pas maximal x0,5.
TEST(MotionAe, SaturatedImageHalvesExposure) {
  const auto e = ae({1000, 16}, 255);
  EXPECT_EQ(e.lines, 500u);
}

// Pose déjà au maximum : la suite passe par le gain (x2 -> 32/16).
TEST(MotionAe, PastMaxLinesUsesGain) {
  const auto e = ae({kMaxLines, 16}, kBlack);
  EXPECT_EQ(e.lines, kMaxLines);
  EXPECT_EQ(e.gain_x16, 32u);
}

TEST(MotionAe, GainIsClamped) {
  const auto e = ae({kMaxLines, 200}, kBlack);
  EXPECT_EQ(e.gain_x16, kMaxGain);
}

// Retour à la lumière depuis un fort gain : le gain redescend avant la pose.
TEST(MotionAe, BrightSceneDropsGainFirst) {
  const auto e = ae({kMaxLines, 64}, 255);
  EXPECT_EQ(e.lines, kMaxLines);
  EXPECT_EQ(e.gain_x16, 32u);
}

TEST(MotionAe, PoseNeverBelowMinimum) {
  const auto e = ae({kMinLines, 16}, 255);
  EXPECT_EQ(e.lines, kMinLines);
}
