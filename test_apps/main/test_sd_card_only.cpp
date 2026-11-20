/*
 * Cycle « matériel seul » de la carte SD, la partie du mode clé USB qui touche
 * au partage de contrôleur SDMMC.
 *
 * Ne couvre PAS la cohabitation avec esp_hosted : test_apps embarque le BSP
 * sans lui (aucun réseau monté). Ce fichier vérifie la correction de
 * l'init/deinit, pas la contention avec le slot 1 — celle-là ne se valide que
 * sur l'appareil complet.
 */
#include "unity.h"

#include "bsp.h"

#include <cstdio>

TEST_CASE("sd_init_card_only round-trips back to a working filesystem",
          "[sd][usb_msc]") {
  bsp::Bsp &bsp = bsp::Bsp::instance();

  // Le BSP monte la carte à l'init : on part de l'état nominal.
  TEST_ASSERT_EQUAL(ESP_OK, bsp.sd_unmount());

  void *card = nullptr;
  TEST_ASSERT_EQUAL(ESP_OK, bsp.sd_init_card_only(&card));
  TEST_ASSERT_NOT_NULL(card);

  TEST_ASSERT_EQUAL(ESP_OK, bsp.sd_deinit_card_only(card));

  // Le système de fichiers doit revenir : c'est ce que fait la sortie du mode.
  TEST_ASSERT_EQUAL(ESP_OK, bsp.sd_mount());
  uint64_t total = 0, freeb = 0;
  TEST_ASSERT_TRUE(bsp.sd_capacity(&total, &freeb));
  // PAS TEST_ASSERT_GREATER_THAN : la macro compare en int 32 bits, et une
  // carte de 64 Go (~62,5e9 octets) y devient negative par troncature — le
  // test echouait sur une capacite parfaitement valide.
  TEST_ASSERT_TRUE(total > 0);
  TEST_ASSERT_TRUE(freeb <= total);

  // Et il doit être réellement utilisable, pas seulement « monté ».
  FILE *f = fopen("/sdcard/usbmsc_probe.txt", "w");
  TEST_ASSERT_NOT_NULL(f);
  TEST_ASSERT_EQUAL(5, fprintf(f, "hello"));
  fclose(f);
  TEST_ASSERT_EQUAL(0, remove("/sdcard/usbmsc_probe.txt"));
}

TEST_CASE("sd_init_card_only refuses to run while the filesystem is mounted",
          "[sd][usb_msc]") {
  bsp::Bsp &bsp = bsp::Bsp::instance();
  TEST_ASSERT_EQUAL(ESP_OK, bsp.sd_mount()); // idempotent si déjà monté

  void *card = nullptr;
  // Deux propriétaires du même slot = corruption. Le refus est la garantie.
  TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, bsp.sd_init_card_only(&card));
  TEST_ASSERT_NULL(card);
}

TEST_CASE("sd_deinit_card_only rejects a null handle", "[sd][usb_msc]") {
  TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                    bsp::Bsp::instance().sd_deinit_card_only(nullptr));
}
