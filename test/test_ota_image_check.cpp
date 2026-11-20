#include <gtest/gtest.h>

#include "sys/ota_image_check.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Vecteur construit À LA MAIN depuis la disposition documentée dans le header,
// jamais produit en exécutant le code testé — un vecteur généré par
// l'implémentation validerait aussi ses bugs (règle du projet).
std::vector<uint8_t> MakeImage(uint8_t img_magic = ota_image::kImageMagic,
                               uint32_t desc_magic = ota_image::kAppDescMagic) {
  std::vector<uint8_t> b(ota_image::kHeaderProbeBytes, 0);
  b[0] = img_magic;
  const size_t d = ota_image::kAppDescOffset;
  b[d + 0] = (uint8_t)(desc_magic & 0xFF);
  b[d + 1] = (uint8_t)((desc_magic >> 8) & 0xFF);
  b[d + 2] = (uint8_t)((desc_magic >> 16) & 0xFF);
  b[d + 3] = (uint8_t)((desc_magic >> 24) & 0xFF);
  memcpy(&b[d + 16], "1.4.2", 5);
  memcpy(&b[d + 48], "visiophone", 10);
  memcpy(&b[d + 80], "12:30:00", 8);
  memcpy(&b[d + 96], "Aug 15 2026", 11);
  return b;
}

// Le vecteur ci-dessus n'est qu'une relecture des décalages écrits dans le
// header : s'ils étaient tous faux du même montant, les tests passeraient quand
// même. Celui-ci est la seule preuve extérieure — 144 octets pris tels quels en
// tête d'un binaire réellement produit par ce projet.
std::vector<uint8_t> LoadRealHeader() {
  std::vector<uint8_t> b;
  FILE *f = fopen(VECTOR_DIR "/app_header.bin", "rb");
  if (f == nullptr) {
    return b;
  }
  b.resize(144);
  const size_t n = fread(b.data(), 1, b.size(), f);
  fclose(f);
  b.resize(n);
  return b;
}

} // namespace

TEST(OtaImageCheck, ImageValideExtraitLesChamps) {
  auto b = MakeImage();
  ota_image::Info info = {};
  ASSERT_TRUE(ota_image::parse_header(b.data(), b.size(), &info));
  EXPECT_STREQ(info.version, "1.4.2");
  EXPECT_STREQ(info.project, "visiophone");
  EXPECT_STREQ(info.date, "Aug 15 2026");
}

TEST(OtaImageCheck, MauvaisMagicImageRefuse) {
  auto b = MakeImage(0x7F);
  ota_image::Info info = {};
  EXPECT_FALSE(ota_image::parse_header(b.data(), b.size(), &info));
}

TEST(OtaImageCheck, MauvaisMagicDescripteurRefuse) {
  auto b = MakeImage(ota_image::kImageMagic, 0xDEADBEEF);
  ota_image::Info info = {};
  EXPECT_FALSE(ota_image::parse_header(b.data(), b.size(), &info));
}

TEST(OtaImageCheck, TamponTropCourtRefuse) {
  auto b = MakeImage();
  ota_image::Info info = {};
  EXPECT_FALSE(ota_image::parse_header(b.data(), b.size() - 1, &info));
  EXPECT_FALSE(ota_image::parse_header(b.data(), 0, &info));
}

TEST(OtaImageCheck, ChampNonTermineEstBorne) {
  auto b = MakeImage();
  // 32 octets de 'A' dans version[] : aucun '\0' dans le champ source.
  memset(&b[ota_image::kAppDescOffset + 16], 'A', 32);
  ota_image::Info info = {};
  ASSERT_TRUE(ota_image::parse_header(b.data(), b.size(), &info));
  EXPECT_EQ(strlen(info.version), 31u);
  EXPECT_EQ(info.version[31], '\0');
}

TEST(OtaImageCheck, ArgumentsNulsRefuses) {
  auto b = MakeImage();
  ota_image::Info info = {};
  EXPECT_FALSE(ota_image::parse_header(nullptr, b.size(), &info));
  EXPECT_FALSE(ota_image::parse_header(b.data(), b.size(), nullptr));
}

TEST(OtaImageCheck, EnteteDUnBinaireReelDuProjet) {
  const auto b = LoadRealHeader();
  ASSERT_EQ(b.size(), 144u) << "test/vectors/app_header.bin introuvable";
  ota_image::Info info = {};
  ASSERT_TRUE(ota_image::parse_header(b.data(), b.size(), &info));
  // Le nom de projet est stable d'une compilation à l'autre, contrairement à
  // la version (dérivée de git describe) et à la date.
  EXPECT_STREQ(info.project, "esp32_p4_webrtc_cam");
  EXPECT_GT(strlen(info.version), 0u);
  EXPECT_GT(strlen(info.date), 0u);
}
