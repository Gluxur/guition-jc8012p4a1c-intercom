#include "esp_timer.h"
#include "test_helpers.h"
#include "unity.h"

#include <cstdio>
#include <cstdlib>

namespace {

// Bornes larges : la conversion YCbCr->RGB en BT.601 et le sous-échantillonnage
// 4:2:0 déplacent les composantes de quelques pas. On vérifie qu'un canal est
// franchement haut ou franchement bas, pas une valeur exacte — ce qui suffit à
// détecter le byte-swap (l'ordre RGB au lieu de BGR intervertit R et B, et
// éclate le vert sur les deux octets).
void expect_color(const char *what, uint16_t px, int r_min, int r_max,
                  int g_min, int g_max, int b_min, int b_max) {
  char msg[128];
  snprintf(msg, sizeof(msg), "%s: px=0x%04X r5=%d g6=%d b5=%d", what, px,
           tst::r5(px), tst::g6(px), tst::b5(px));
  TEST_ASSERT_TRUE_MESSAGE(tst::r5(px) >= r_min && tst::r5(px) <= r_max, msg);
  TEST_ASSERT_TRUE_MESSAGE(tst::g6(px) >= g_min && tst::g6(px) <= g_max, msg);
  TEST_ASSERT_TRUE_MESSAGE(tst::b5(px) >= b_min && tst::b5(px) <= b_max, msg);
}

// Les quatre quadrants, échantillonnés bien à l'intérieur pour éviter le
// ringing JPEG sur les fronts.
void check_quadrants(const tst::Decoded &d) {
  const uint32_t qx = d.width / 4;
  const uint32_t qy = d.height / 4;
  expect_color("haut-gauche rouge", tst::pixel_at(d, qx, qy), 24, 31, 0, 12, 0, 10);
  expect_color("haut-droit vert", tst::pixel_at(d, d.width - qx, qy), 0, 10, 48, 63, 0, 10);
  expect_color("bas-gauche bleu", tst::pixel_at(d, qx, d.height - qy), 0, 10, 0, 12, 24, 31);
  expect_color("bas-droit blanc", tst::pixel_at(d, d.width - qx, d.height - qy), 26, 31, 52, 63, 26, 31);
}

} // namespace

TEST_CASE("JPEG HW: l'entete donne les dimensions exactes", "[jpeg]") {
  TEST_ASSERT_NOT_NULL_MESSAGE(tst::decoder(), "moteur JPEG non cree au boot");

  jpeg_decode_picture_info_t info = {};
  TEST_ASSERT_EQUAL(ESP_OK,
                    jpeg_decoder_get_info(k320_start, len320(), &info));
  TEST_ASSERT_EQUAL_UINT32(320, info.width);
  TEST_ASSERT_EQUAL_UINT32(240, info.height);

  info = {};
  TEST_ASSERT_EQUAL(ESP_OK,
                    jpeg_decoder_get_info(k200_start, len200(), &info));
  TEST_ASSERT_EQUAL_UINT32(200, info.width);
  TEST_ASSERT_EQUAL_UINT32(120, info.height);
}

TEST_CASE("JPEG HW: la sortie RGB565 a les bonnes couleurs (ordre BGR)",
          "[jpeg]") {
  tst::Decoded d;
  TEST_ASSERT_TRUE(tst::decode_asset(k320_start, len320(), &d));
  check_quadrants(d);
  tst::free_decoded(&d);
}

TEST_CASE("JPEG HW: la sortie est alignee sur 16 px", "[jpeg]") {
  // 200x120 -> le moteur ecrit 208x128. C'est le piege documente : dimensionner
  // ou parcourir la sortie sur 200 px de large donne une image tuilee/cisaillee,
  // parce que chaque ligne demarre 8 px trop tot.
  tst::Decoded d;
  TEST_ASSERT_TRUE(tst::decode_asset(k200_start, len200(), &d));

  TEST_ASSERT_EQUAL_UINT32(208, d.stride);
  // Preuve directe de l'alignement : le moteur annonce lui-meme avoir ecrit le
  // rectangle aligne, pas le rectangle utile.
  TEST_ASSERT_EQUAL_UINT32(208u * 128u * 2u, d.decoded_bytes);

  // Et les couleurs restent justes quand on parcourt AVEC ce stride : une
  // lecture a 200 px derivereait de 8 px par ligne.
  check_quadrants(d);
  tst::free_decoded(&d);
}

TEST_CASE("JPEG HW: un buffer dimensionne sur w x h non aligne est refuse",
          "[jpeg]") {
  TEST_ASSERT_NOT_NULL(tst::decoder());

  // 200*120*2 = 48000 o, alors que le moteur veut ecrire 208*128*2 = 53248 o.
  jpeg_decode_memory_alloc_cfg_t mem = {JPEG_DEC_ALLOC_OUTPUT_BUFFER};
  size_t got = 0;
  auto *buf = static_cast<uint8_t *>(
      jpeg_alloc_decoder_mem(200u * 120u * 2u, &mem, &got));
  TEST_ASSERT_NOT_NULL(buf);

  jpeg_decode_cfg_t cfg = {};
  cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
  cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;

  uint32_t out_size = 0;
  // On passe volontairement la taille NON alignee, pas `got` (que l'allocateur
  // a pu arrondir au-dela).
  esp_err_t err = jpeg_decoder_process(tst::decoder(), &cfg, k200_start,
                                       len200(), buf, 200u * 120u * 2u,
                                       &out_size);
  free(buf);

  char msg[96];
  snprintf(msg, sizeof(msg), "attendu un echec, obtenu %s",
           esp_err_to_name(err));
  TEST_ASSERT_TRUE_MESSAGE(err != ESP_OK, msg);
}

TEST_CASE("JPEG HW: le decodage tient le budget temps d'une frame", "[jpeg]") {
  // Le pipeline vise ~22-25 fps, soit 40 ms par frame partagees entre drain
  // TCP, decodage et rendu. Une 320x240 est bien plus petite qu'une frame
  // camera (800x640, ~8,5 ms mesures) : si elle depasse 15 ms, le moteur ne
  // tourne pas a la vitesse attendue.
  tst::Decoded d;
  const int64_t t0 = esp_timer_get_time();
  TEST_ASSERT_TRUE(tst::decode_asset(k320_start, len320(), &d));
  const int64_t us = esp_timer_get_time() - t0;
  tst::free_decoded(&d);

  // Inclut l'allocation du buffer de sortie, d'ou la marge.
  char msg[96];
  snprintf(msg, sizeof(msg), "decodage 320x240 en %lld us", us);
  printf("%s\n", msg);
  TEST_ASSERT_TRUE_MESSAGE(us < 15000, msg);
}
