#include "media/wav_parse.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

// Constructeurs de vecteurs écrits depuis la spec RIFF/WAVE (Microsoft), PAS
// depuis wav_parse.cpp : un en-tête produit en exécutant le parser validerait
// aussi ses bugs. Disposition, en little-endian :
//   "RIFF" | taille-4 | "WAVE"                     -> 12 octets
//   puis, en boucle : id (4) | taille (4) | données (taille) [+ 1 si impaire]
//   fmt PCM (16 o) : format u16 | voies u16 | freq u32 | debit u32 |
//                    align u16 | bits u16
void put_u16(std::vector<uint8_t> &v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x & 0xFF));
  v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}

void put_u32(std::vector<uint8_t> &v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x & 0xFF));
  v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
  v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
  v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}

void put_id(std::vector<uint8_t> &v, const char *id) {
  for (int i = 0; i < 4; i++)
    v.push_back(static_cast<uint8_t>(id[i]));
}

struct Fmt {
  uint16_t format = 1; // 1 = PCM entier, 3 = IEEE float
  uint16_t channels = 1;
  uint32_t rate = 8000;
  uint16_t bits = 16;
};

// `extra_id`/`extra_bytes` insèrent un chunk quelconque entre `fmt ` et `data`
// (le cas LIST/INFO des fichiers réels). `riff_id` permet de forger un RIFX.
std::vector<uint8_t> make_wav(const Fmt &f, uint32_t data_bytes,
                              const char *extra_id = nullptr,
                              uint32_t extra_bytes = 0,
                              const char *riff_id = "RIFF",
                              const char *wave_id = "WAVE") {
  std::vector<uint8_t> v;
  put_id(v, riff_id);
  put_u32(v, 0); // taille globale : jamais lue par le parser
  put_id(v, wave_id);

  put_id(v, "fmt ");
  put_u32(v, 16);
  put_u16(v, f.format);
  put_u16(v, f.channels);
  put_u32(v, f.rate);
  put_u32(v, f.rate * f.channels * (f.bits / 8u)); // débit octets/s
  put_u16(v, static_cast<uint16_t>(f.channels * (f.bits / 8u)));
  put_u16(v, f.bits);

  if (extra_id != nullptr) {
    put_id(v, extra_id);
    put_u32(v, extra_bytes);
    for (uint32_t i = 0; i < extra_bytes; i++)
      v.push_back(0xAA);
    if (extra_bytes & 1u)
      v.push_back(0x00); // bourrage RIFF des chunks de taille impaire
  }

  put_id(v, "data");
  put_u32(v, data_bytes);
  // Le corps du bloc data n'est pas nécessaire : parse() ne lit qu'un en-tête.
  return v;
}

// --- Cas nominal ------------------------------------------------------------

TEST(WavParse, AcceptsAMinimal8kHzMonoPcm16Header) {
  // Offsets calculés à la main depuis la spec :
  //   RIFF+taille+WAVE = 12 ; "fmt "+taille = 8 ; corps fmt = 16  -> 36
  //   "data"+taille = 8                                          -> 44
  const std::vector<uint8_t> v = make_wav(Fmt{}, 16000);
  wav::Info info;
  EXPECT_EQ(wav::Error::kOk, wav::parse(v.data(), v.size(), info));
  EXPECT_EQ(44u, info.data_offset);
  EXPECT_EQ(16000u, info.data_bytes);
  EXPECT_EQ(8000u, info.sample_rate);
  EXPECT_EQ(1u, info.channels);
  EXPECT_EQ(16u, info.bits);
}

TEST(WavParse, DataMayBeLargerThanTheHeaderWindow) {
  // `data` est le SEUL chunk autorisé à déclarer plus que ce que la fenêtre
  // contient : ringtone_sd ne lit que les 4 premiers Ko du fichier.
  const std::vector<uint8_t> v = make_wav(Fmt{}, 480000);
  wav::Info info;
  EXPECT_EQ(wav::Error::kOk, wav::parse(v.data(), v.size(), info));
  EXPECT_EQ(480000u, info.data_bytes);
}

// --- Parcours de chunks -----------------------------------------------------

TEST(WavParse, SkipsAListChunkBetweenFmtAndData) {
  // LE cas qui distingue un parcours de chunks d'une lecture à offset fixe :
  // ffmpeg insère systématiquement un LIST/INFO. Un parser à 44 octets en dur
  // passerait ce test-ci en lisant n'importe quoi.
  //   36 (fin fmt) + 8 (en-tête LIST) + 10 (corps, pair) = 54 ; +8 -> 62
  const std::vector<uint8_t> v = make_wav(Fmt{}, 16000, "LIST", 10);
  wav::Info info;
  EXPECT_EQ(wav::Error::kOk, wav::parse(v.data(), v.size(), info));
  EXPECT_EQ(62u, info.data_offset);
}

TEST(WavParse, HonoursThePadByteAfterAnOddSizedChunk) {
  // RIFF impose un octet de bourrage après tout chunk de taille impaire ; il
  // ne compte PAS dans la taille déclarée. L'oublier décale tout ce qui suit.
  //   36 + 8 + 5 = 49, +1 de bourrage = 50 ; +8 -> 58
  const std::vector<uint8_t> v = make_wav(Fmt{}, 16000, "LIST", 5);
  wav::Info info;
  EXPECT_EQ(wav::Error::kOk, wav::parse(v.data(), v.size(), info));
  EXPECT_EQ(58u, info.data_offset);
}

TEST(WavParse, RejectsAChunkLongerThanTheBuffer) {
  // Une carte SD est amovible : le fichier peut être corrompu ou forgé. Suivre
  // une longueur non vérifiée ferait sortir le parcours du tampon.
  std::vector<uint8_t> v = make_wav(Fmt{}, 16000, "LIST", 4);
  // Réécrit la taille du chunk LIST (offset 36 + 4 = 40) à 0xFFFFFF00.
  v[40] = 0x00;
  v[41] = 0xFF;
  v[42] = 0xFF;
  v[43] = 0xFF;
  wav::Info info;
  EXPECT_EQ(wav::Error::kTruncated, wav::parse(v.data(), v.size(), info));
}

TEST(WavParse, RejectsAFmtChunkLongerThanTheBuffer) {
  // Sur la cible (size_t 32 bits), 0xFFFFFFF8 faisait reboucler le curseur sur
  // ce même chunk : boucle infinie au boot, reboot en boucle tant que le
  // fichier restait sur la carte. Le host est en 64 bits et ne reboucle pas —
  // d'où l'exigence d'un refus explicite, testable ici, plutôt que de compter
  // sur la sortie de boucle.
  std::vector<uint8_t> v = make_wav(Fmt{}, 16000);
  // Taille du chunk fmt : offset 12 + 4 = 16.
  v[16] = 0xF8;
  v[17] = 0xFF;
  v[18] = 0xFF;
  v[19] = 0xFF;
  wav::Info info;
  EXPECT_EQ(wav::Error::kTruncated, wav::parse(v.data(), v.size(), info));
}

// --- Format refusé ----------------------------------------------------------

TEST(WavParse, RejectsANonEightKilohertzRate) {
  Fmt f;
  f.rate = 44100;
  const std::vector<uint8_t> v = make_wav(f, 16000);
  wav::Info info;
  EXPECT_EQ(wav::Error::kBadRate, wav::parse(v.data(), v.size(), info));
  // La fréquence fautive doit remonter : c'est elle que le journal affiche.
  EXPECT_EQ(44100u, info.sample_rate);
}

TEST(WavParse, RejectsStereo) {
  Fmt f;
  f.channels = 2;
  const std::vector<uint8_t> v = make_wav(f, 16000);
  wav::Info info;
  EXPECT_EQ(wav::Error::kNotMono, wav::parse(v.data(), v.size(), info));
  EXPECT_EQ(2u, info.channels);
}

TEST(WavParse, RejectsEightBitSamples) {
  Fmt f;
  f.bits = 8;
  const std::vector<uint8_t> v = make_wav(f, 16000);
  wav::Info info;
  EXPECT_EQ(wav::Error::kNotPcm16, wav::parse(v.data(), v.size(), info));
  EXPECT_EQ(8u, info.bits);
}

TEST(WavParse, RejectsIeeeFloatSamples) {
  Fmt f;
  f.format = 3; // WAVE_FORMAT_IEEE_FLOAT
  f.bits = 32;
  const std::vector<uint8_t> v = make_wav(f, 16000);
  wav::Info info;
  EXPECT_EQ(wav::Error::kNotPcm16, wav::parse(v.data(), v.size(), info));
}

// --- Conteneur invalide -----------------------------------------------------

TEST(WavParse, RejectsBigEndianRifx) {
  const std::vector<uint8_t> v = make_wav(Fmt{}, 16000, nullptr, 0, "RIFX");
  wav::Info info;
  EXPECT_EQ(wav::Error::kNotRiff, wav::parse(v.data(), v.size(), info));
}

TEST(WavParse, RejectsRiffThatIsNotWave) {
  const std::vector<uint8_t> v =
      make_wav(Fmt{}, 16000, nullptr, 0, "RIFF", "AVI ");
  wav::Info info;
  EXPECT_EQ(wav::Error::kNotWave, wav::parse(v.data(), v.size(), info));
}

TEST(WavParse, RejectsArbitraryBytes) {
  const std::vector<uint8_t> v(64, 0x5A);
  wav::Info info;
  EXPECT_EQ(wav::Error::kNotRiff, wav::parse(v.data(), v.size(), info));
}

TEST(WavParse, RejectsABufferShorterThanTheRiffHeader) {
  const std::vector<uint8_t> v = {'R', 'I', 'F', 'F', 0, 0};
  wav::Info info;
  EXPECT_EQ(wav::Error::kTruncated, wav::parse(v.data(), v.size(), info));
}

TEST(WavParse, RejectsAHeaderCutInsideFmt) {
  std::vector<uint8_t> v = make_wav(Fmt{}, 16000);
  // Le corps de `fmt ` occupe les octets 20 à 35 : couper à 30 tombe dedans.
  // Couper à 40 ne testerait PAS ce que dit le nom du test — `fmt ` serait
  // complet et l'erreur remontée serait kNoData.
  v.resize(30);
  wav::Info info;
  EXPECT_EQ(wav::Error::kTruncated, wav::parse(v.data(), v.size(), info));
}

TEST(WavParse, RejectsAFileWithoutDataChunk) {
  std::vector<uint8_t> v = make_wav(Fmt{}, 16000);
  v.resize(36); // tout `fmt `, rien après
  wav::Info info;
  EXPECT_EQ(wav::Error::kNoData, wav::parse(v.data(), v.size(), info));
}

TEST(WavParse, RejectsAnEmptyDataChunk) {
  const std::vector<uint8_t> v = make_wav(Fmt{}, 0);
  wav::Info info;
  EXPECT_EQ(wav::Error::kNoData, wav::parse(v.data(), v.size(), info));
}

TEST(WavParse, RejectsDataWithoutAPrecedingFmt) {
  std::vector<uint8_t> v;
  put_id(v, "RIFF");
  put_u32(v, 0);
  put_id(v, "WAVE");
  put_id(v, "data");
  put_u32(v, 16000);
  wav::Info info;
  EXPECT_EQ(wav::Error::kNoFmt, wav::parse(v.data(), v.size(), info));
}

TEST(WavParse, RejectsANullBuffer) {
  wav::Info info;
  EXPECT_EQ(wav::Error::kTruncated, wav::parse(nullptr, 128, info));
}

// --- Libellés ---------------------------------------------------------------

TEST(WavParse, EveryErrorHasANonEmptyAsciiSafeLabel) {
  // Ces libellés s'affichent sur la page Configuration en montserrat_fr, dont
  // le jeu de glyphes s'arrête à 0xFF : un tiret cadratin sortirait en carré.
  const wav::Error all[] = {
      wav::Error::kOk,      wav::Error::kTruncated, wav::Error::kNotRiff,
      wav::Error::kNotWave, wav::Error::kNoFmt,     wav::Error::kNotPcm16,
      wav::Error::kBadRate, wav::Error::kNotMono,   wav::Error::kNoData,
  };
  for (wav::Error e : all) {
    const char *t = wav::error_text(e);
    ASSERT_NE(nullptr, t);
    EXPECT_GT(strlen(t), 0u);
  }
}

} // namespace
