#include "bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "test_helpers.h"
#include "test_menu.h"
#include "unity.h"

#include <cstdio>

namespace {
constexpr const char *TAG = "test_app";
}

extern "C" void app_main(void) {
  // Même ordre qu'app_main du firmware : NVS d'abord, le BSP relit
  // volume/gain/rétroéclairage depuis NVS dans load_prefs().
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    (void)nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    // Pas d'ESP_ERROR_CHECK (cf. CLAUDE.md) : on veut voir les autres tests
    // tourner même si NVS est perdu.
    ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
  }

  // Le moteur JPEG exige de la RAM interne DMA : créé au boot, comme en
  // production, pas à la demande dans un test.
  if (!tst::init_decoder()) {
    ESP_LOGE(TAG, "moteur JPEG indisponible, les tests [jpeg] vont échouer");
  }

  err = bsp::Bsp::instance().init();
  tst::set_bsp_ready(err == ESP_OK);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "bsp init: %s — les tests [bsp] vont échouer",
             esp_err_to_name(err));
  }

  printf("\n\n"
         "#############################################\n"
         "  Tests sur cible - visiophone ESP32-P4\n"
         "  '0' lance toute la suite.\n"
         "#############################################\n");
  tst::run_menu();
}
