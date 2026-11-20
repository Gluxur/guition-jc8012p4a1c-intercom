#include "bsp.h"
#include "esp_timer.h"
#include "test_helpers.h"
#include "unity.h"

#include <cstdio>
#include <cstring>

namespace {

// 20 ms a 8 kHz mono : la taille de trame du backchannel micro.
constexpr size_t kFrameSamples = 160;

void require_bsp() {
  TEST_ASSERT_TRUE_MESSAGE(tst::bsp_ready(),
                           "BSP non initialise au boot, test impossible");
}

} // namespace

TEST_CASE("ES8311: le micro delivre bien 8000 echantillons/s", "[audio]") {
  require_bsp();
  auto &bsp = bsp::Bsp::instance();
  int16_t pcm[kFrameSamples] = {};

  TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, bsp.read_pcm(pcm, kFrameSamples),
                            "codec absent ou canal I2S RX mort");

  // Vidange obligatoire avant toute mesure. Le canal RX tourne depuis l'init du
  // BSP : quand ce test demarre, la ring DMA est deja pleine (tailles par
  // defaut du driver, 6 descripteurs x 240 trames = 1440 echantillons, soit
  // ~180 ms a 8 kHz). Les premieres lectures reviennent de ce backlog en
  // quelques dizaines de microsecondes et ne mesurent RIEN — c'est ce qui
  // faisait passer une mesure de cadence pour un codec muet.
  constexpr int kMaxDrain = 40;
  int drained = 0;
  for (; drained < kMaxDrain; ++drained) {
    const int64_t t0 = esp_timer_get_time();
    TEST_ASSERT_EQUAL(ESP_OK, bsp.read_pcm(pcm, kFrameSamples));
    if (esp_timer_get_time() - t0 > 5000) {
      break; // la lecture attend le codec : cadence nominale atteinte
    }
  }
  TEST_ASSERT_TRUE_MESSAGE(drained < kMaxDrain,
                           "le backlog DMA ne se vide jamais");

  // En regime etabli, chaque lecture est cadencee par l'horloge du codec.
  constexpr int kFrames = 10; // 1600 echantillons = 200 ms attendues
  const int64_t t0 = esp_timer_get_time();
  for (int i = 0; i < kFrames; ++i) {
    TEST_ASSERT_EQUAL(ESP_OK, bsp.read_pcm(pcm, kFrameSamples));
  }
  const int64_t us = esp_timer_get_time() - t0;
  TEST_ASSERT_TRUE_MESSAGE(us > 0, "horloge figee");

  const long rate =
      static_cast<long>(kFrames * kFrameSamples * 1000000LL / us);
  char msg[112];
  snprintf(msg, sizeof(msg), "%d trames en %lld us -> %ld ech/s (attendu 8000)",
           kFrames, us, rate);
  printf("%s\n", msg);

  // +/-15 % : couvre la gigue d'ordonnancement sur 200 ms, pas une erreur de
  // diviseur d'horloge (qui donnerait un facteur 2 ou 6).
  TEST_ASSERT_TRUE_MESSAGE(rate > 6800 && rate < 9200, msg);
}

TEST_CASE("ES8311: une trame silencieuse s'ecrit au haut-parleur", "[audio]") {
  require_bsp();
  int16_t silence[kFrameSamples];
  memset(silence, 0, sizeof(silence));

  TEST_ASSERT_EQUAL_MESSAGE(ESP_OK,
                            bsp::Bsp::instance().play_pcm(silence, kFrameSamples),
                            "codec absent ou canal I2S TX mort");
}

TEST_CASE("BSP: le volume haut-parleur est borne a 100", "[bsp]") {
  require_bsp();
  auto &bsp = bsp::Bsp::instance();

  // set_speaker_volume ecrit en NVS : on restaure la valeur de l'utilisateur,
  // sinon lancer les tests reglerait le volume du visiophone au passage.
  const uint8_t original = bsp.speaker_volume();

  bsp.set_speaker_volume(200);
  TEST_ASSERT_EQUAL_UINT8(100, bsp.speaker_volume());

  bsp.set_speaker_volume(42);
  TEST_ASSERT_EQUAL_UINT8(42, bsp.speaker_volume());

  bsp.set_speaker_volume(original);
  TEST_ASSERT_EQUAL_UINT8(original, bsp.speaker_volume());
}

TEST_CASE("BSP: le retroeclairage est borne a 100", "[bsp]") {
  require_bsp();
  auto &bsp = bsp::Bsp::instance();
  const uint8_t original = bsp.backlight();

  bsp.set_backlight(200);
  TEST_ASSERT_EQUAL_UINT8(100, bsp.backlight());

  bsp.set_backlight(original);
  TEST_ASSERT_EQUAL_UINT8(original, bsp.backlight());
}
