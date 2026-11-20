#include "bsp.h"
#include "test_helpers.h"
#include "unity.h"

#include <cstdio>

namespace {

constexpr uint32_t kHRes = 800;  // bsp::Bsp::kLcdHRes
constexpr uint32_t kVRes = 1280; // bsp::Bsp::kLcdVRes

void require_bsp() {
  TEST_ASSERT_TRUE_MESSAGE(tst::bsp_ready(),
                           "BSP non initialise au boot, test impossible");
}

} // namespace

TEST_CASE("BSP: l'init a bien arme panneau, LVGL et framebuffers", "[bsp]") {
  require_bsp();
  auto &bsp = bsp::Bsp::instance();
  TEST_ASSERT_NOT_NULL(bsp.display());
  TEST_ASSERT_NOT_NULL(bsp.panel());
  TEST_ASSERT_NOT_NULL(bsp.touch_indev());
  TEST_ASSERT_NOT_NULL(bsp.dsi_frame_buffer());
}

TEST_CASE("BSP: les deux framebuffers DSI existent et sont distincts", "[bsp]") {
  // Piege documente : en mode PARTIAL, LVGL n'ecrit que dans le buffer scanne.
  // Toute zone que la video ne repeint pas doit etre nettoyee dans LES DEUX,
  // sinon l'ecran precedent reapparait a chaque bascule. Ce test garantit au
  // moins qu'il y a bien deux buffers a maintenir.
  require_bsp();
  auto &bsp = bsp::Bsp::instance();
  void *fb0 = bsp.dsi_frame_buffer_at(0);
  void *fb1 = bsp.dsi_frame_buffer_at(1);
  TEST_ASSERT_NOT_NULL(fb0);
  TEST_ASSERT_NOT_NULL(fb1);
  TEST_ASSERT_TRUE_MESSAGE(fb0 != fb1, "un seul framebuffer, double buffer perdu");
}

TEST_CASE("BSP: direct_video_target accepte la largeur panneau sans echelle",
          "[bsp]") {
  require_bsp();
  auto &bsp = bsp::Bsp::instance();
  auto *fb = static_cast<uint8_t *>(bsp.dsi_frame_buffer());
  TEST_ASSERT_NOT_NULL(fb);

  size_t out_size = 0;
  uint8_t *dst = bsp.direct_video_target(800, 640, 0, 0, kHRes, kVRes, &out_size);

  TEST_ASSERT_NOT_NULL_MESSAGE(dst, "chemin zero copie refuse a tort");
  TEST_ASSERT_EQUAL_UINT32(800u * 640u * 2u, out_size);
  // Centrage vertical sur ligne paire : y0 = (1280-640)/2 = 320.
  TEST_ASSERT_EQUAL_PTR(fb + static_cast<size_t>(320) * kHRes * 2, dst);

  uint32_t x, y, w, h;
  bsp.video_block_rect(x, y, w, h);
  TEST_ASSERT_EQUAL_UINT32(0, x);
  TEST_ASSERT_EQUAL_UINT32(320, y);
  TEST_ASSERT_EQUAL_UINT32(800, w);
  TEST_ASSERT_EQUAL_UINT32(640, h);
}

TEST_CASE("BSP: direct_video_target refuse toute geometrie hors contrat",
          "[bsp]") {
  require_bsp();
  auto &bsp = bsp::Bsp::instance();
  size_t sz = 0;

  // Hauteur non alignee 16 : le decodeur ecrirait des lignes de bourrage au
  // contenu indefini directement a l'ecran.
  TEST_ASSERT_NULL(bsp.direct_video_target(800, 630, 0, 0, kHRes, kVRes, &sz));
  // Largeur non alignee 16, meme raison.
  TEST_ASSERT_NULL(bsp.direct_video_target(792, 640, 0, 0, kHRes, kVRes, &sz));
  // Largeur differente de celle du panneau : les lignes decodees ne tombent
  // plus sur les lignes du framebuffer, il faut repasser par le PPA.
  TEST_ASSERT_NULL(bsp.direct_video_target(640, 480, 0, 0, kHRes, kVRes, &sz));
  // Zone qui ne commence pas au bord gauche.
  TEST_ASSERT_NULL(bsp.direct_video_target(800, 640, 32, 0, kHRes, kVRes, &sz));
  // Source plus haute que la zone cible.
  TEST_ASSERT_NULL(bsp.direct_video_target(800, 640, 0, 0, kHRes, 320, &sz));
  // Zone qui deborde du bas du panneau.
  TEST_ASSERT_NULL(bsp.direct_video_target(800, 640, 0, 800, kHRes, 640, &sz));
  // Sans receptacle pour la taille, l'appelant ne saurait pas quoi passer a
  // jpeg_decoder_process.
  TEST_ASSERT_NULL(
      bsp.direct_video_target(800, 640, 0, 0, kHRes, kVRes, nullptr));
}

TEST_CASE("BSP: le rendu PPA respecte l'aspect et le centrage", "[bsp]") {
  require_bsp();
  auto &bsp = bsp::Bsp::instance();

  tst::Decoded d;
  TEST_ASSERT_TRUE(tst::decode_asset(k320_start, len320(), &d));

  // 320x240 dans 800x1280 : echelle = min(2,5 ; 5,33) = 2,5, quantifiee au
  // 1/16 (40/16, donc exacte) -> 800x600. Horizontal : (800-800)/2 = 0.
  // Vertical : (1280-600)/2 = 340, arrondi a la ligne paire.
  esp_err_t err = bsp.render_rgb565_to_panel(d.buf, d.width, d.height, 0, 0,
                                             kHRes, kVRes, d.stride);
  TEST_ASSERT_EQUAL(ESP_OK, err);

  uint32_t x, y, w, h;
  bsp.video_block_rect(x, y, w, h);
  TEST_ASSERT_EQUAL_UINT32(800, w);
  TEST_ASSERT_EQUAL_UINT32(600, h);
  TEST_ASSERT_EQUAL_UINT32(0, x);
  TEST_ASSERT_EQUAL_UINT32(340, y);

  tst::free_decoded(&d);
}

TEST_CASE("BSP: le PPA video ne perd aucune transaction", "[bsp]") {
  // ppa_video_timeouts() doit rester a 0. Toute valeur non nulle est le
  // symptome AVANT-COUREUR d'un moteur SRM qui se verrouille, c'est-a-dire du
  // gel total de l'appareil (le mode BLOCKING du driver attend sans timeout).
  // Une rafale suffit a faire sortir un moteur instable.
  require_bsp();
  auto &bsp = bsp::Bsp::instance();

  tst::Decoded d;
  TEST_ASSERT_TRUE(tst::decode_asset(k320_start, len320(), &d));

  const uint32_t before = bsp.ppa_video_timeouts();
  for (int i = 0; i < 60; ++i) {
    esp_err_t err = bsp.render_rgb565_to_panel(d.buf, d.width, d.height, 0, 0,
                                               kHRes, kVRes, d.stride);
    if (err != ESP_OK) {
      tst::free_decoded(&d);
      char msg[80];
      snprintf(msg, sizeof(msg), "rendu %d/60 : %s", i, esp_err_to_name(err));
      TEST_FAIL_MESSAGE(msg);
    }
  }
  const uint32_t after = bsp.ppa_video_timeouts();
  tst::free_decoded(&d);

  char msg[96];
  snprintf(msg, sizeof(msg), "transactions PPA perdues : %lu -> %lu",
           static_cast<unsigned long>(before), static_cast<unsigned long>(after));
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(before, after, msg);
}

TEST_CASE("BSP: present_frame fait tourner le double buffer", "[bsp]") {
  require_bsp();
  auto &bsp = bsp::Bsp::instance();

  const int idx = bsp.dsi_write_index();
  void *written = bsp.dsi_frame_buffer();
  TEST_ASSERT_EQUAL_PTR(bsp.dsi_frame_buffer_at(idx), written);

  TEST_ASSERT_EQUAL(ESP_OK, bsp.present_frame());

  // Le buffer qu'on vient de presenter devient celui scanne par le DSI, et les
  // ecritures suivantes visent l'autre.
  TEST_ASSERT_EQUAL_INT(idx ^ 1, bsp.dsi_write_index());
  TEST_ASSERT_EQUAL_PTR(written, bsp.dsi_shown_frame_buffer());
  TEST_ASSERT_EQUAL_PTR(bsp.dsi_frame_buffer_at(idx ^ 1), bsp.dsi_frame_buffer());

  // La bascule n'est qu'ARMEE au retour de present_frame : elle ne prend qu'a
  // la fin de la trame DSI en cours (jusqu'a ~31 ms). Ecrire avant, c'est
  // repeindre le buffer affiche.
  TEST_ASSERT_EQUAL(ESP_OK, bsp.wait_frame_flip(200));
}
