#include "screenshot.h"

#include <atomic>
#include <cstdio>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h" // xTaskCreatePinnedToCoreWithCaps
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "app/camera_session.h"
#include "app_config.h"
#include "bsp.h"
#include "sys/i18n.h"
#include "sys/sd_image.h"
#include "ui/ui.h"

namespace screenshot {
namespace {

constexpr const char *TAG = "screenshot";

enum class Req : uint8_t { kNone, kCurrent, kAll };
std::atomic<Req> s_req{Req::kNone};
SemaphoreHandle_t s_wake = nullptr;

// Après open_page : l'ouverture est postée à lvgl_task, puis l'animation de
// chargement d'écran et les icônes Lottie de la météo doivent se poser.
// Valeur de départ NON mesurée : l'allonger si une capture montre une page à
// moitié dessinée.
constexpr uint32_t kSettleMs = 2500;

bool blocked() {
  return camera_session::active() || !sd_image::sd_ready();
}

const char *lang_code() {
  return i18n::lang() == i18n::Lang::kEn ? "en" : "fr";
}

// lv_snapshot_take rend l'écran entier dans un tampon neuf (2 Mo PSRAM, via
// l'allocateur LVGL) : verrou LVGL le temps du rendu seulement, l'écriture SD
// (~2 Mo, plusieurs secondes) se fait sans le verrou pour ne pas figer l'UI.
bool capture(const char *page) {
  lv_draw_buf_t *snap = nullptr;
  {
    bsp::Bsp::LvglLock lock(1000);
    if (!lock.held()) {
      ESP_LOGW(TAG, "%s : verrou LVGL indisponible", page);
      return false;
    }
    snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
  }
  if (snap == nullptr) {
    ESP_LOGE(TAG, "%s : snapshot impossible (PSRAM ?)", page);
    return false;
  }
  char path[64];
  snprintf(path, sizeof path, "%s/%s_%s.bmp", cfg::sd::kScreenshotDir,
           lang_code(), page);
  const bool ok = sd_image::write_bmp565(
      path, static_cast<const uint8_t *>(snap->data),
      static_cast<int>(snap->header.w), static_cast<int>(snap->header.h),
      snap->header.stride);
  {
    bsp::Bsp::LvglLock lock;
    lv_draw_buf_destroy(snap);
  }
  ESP_LOGI(TAG, "%s : %s", path, ok ? "écrit" : "échec");
  return ok;
}

// make_room avec une limite large : crée le répertoire sans rien supprimer
// (les noms sont fixes, chaque tour écrase le précédent).
bool ensure_dir() {
  return sd_image::make_room(cfg::sd::kScreenshotDir, ".bmp", 64);
}

void run_all() {
  for (const char *page : ui::kPageNames) {
    if (blocked()) {
      ESP_LOGW(TAG, "tour interrompu (appel ou carte SD)");
      return;
    }
    if (!ui::open_page(page)) {
      ESP_LOGW(TAG, "%s : ouverture impossible", page);
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(kSettleMs));
    (void)capture(page);
  }
  (void)ui::open_page("accueil");
}

void screenshot_task(void *) {
  for (;;) {
    xSemaphoreTake(s_wake, portMAX_DELAY);
    const Req req = s_req.load();
    if (blocked()) {
      ESP_LOGW(TAG, "capture refusée : appel en cours ou carte SD indisponible");
    } else if (!ensure_dir()) {
      ESP_LOGE(TAG, "répertoire %s inutilisable", cfg::sd::kScreenshotDir);
    } else if (req == Req::kAll) {
      run_all();
    } else if (req == Req::kCurrent) {
      (void)capture(ui::current_page());
    }
    s_req.store(Req::kNone);
  }
}

void post(Req r) {
  if (s_wake == nullptr) {
    return;
  }
  Req expected = Req::kNone;
  if (!s_req.compare_exchange_strong(expected, r)) {
    ESP_LOGW(TAG, "capture déjà en cours, requête ignorée");
    return;
  }
  xSemaphoreGive(s_wake);
}

} // namespace

void init() {
  s_wake = xSemaphoreCreateBinary();
  if (s_wake == nullptr) {
    ESP_LOGE(TAG, "sémaphore non créé : captures indisponibles");
    return;
  }
  // Cœur 0, priorité 1 : une capture n'est jamais urgente. Pile en PSRAM
  // (règle projet), 8 Ko : IO FATFS (règle projet >= 8 Ko).
  if (xTaskCreatePinnedToCoreWithCaps(screenshot_task, "screenshot", 8192,
                                      nullptr, 1, nullptr, 0,
                                      MALLOC_CAP_SPIRAM) != pdPASS) {
    ESP_LOGE(TAG, "création de la tâche impossible");
    vSemaphoreDelete(s_wake);
    s_wake = nullptr;
  }
}

void request_current() { post(Req::kCurrent); }

void request_all() { post(Req::kAll); }

} // namespace screenshot
