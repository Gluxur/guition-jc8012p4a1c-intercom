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

// Rendu de la capture DANS lvgl_task, jamais ici : avec LV_OS_NONE,
// lv_snapshot_take redessine tout l'arbre d'objets dans la tâche appelante, et
// les icônes Lottie de la page météo passent par ThorVG, dont rleRender() pose
// à lui seul 16 Ko sur la pile (cf. main.cpp, pile de lvgl_task portée à
// 48 Ko pour cette raison). En v2.4.0-beta.1 le rendu se faisait dans cette
// tâche (8 Ko) : la capture de la page météo, et elle seule, redémarrait la
// tablette — cause déduite de ce qui précède, trace de panic non relevée.
//
// s_snap_waiting arbitre l'abandon : celui qui le remet à false le premier
// (le rappel qui livre, ou l'attente qui expire) décide du sort du tampon.
std::atomic<lv_draw_buf_t *> s_snap{nullptr};
std::atomic<bool> s_snap_waiting{false};
SemaphoreHandle_t s_snap_done = nullptr;
// Borne de sécurité, pas une mesure : au-delà, la tâche LVGL est bloquée
// ailleurs et on abandonne cette page plutôt que d'attendre indéfiniment.
constexpr uint32_t kSnapTimeoutMs = 5000;

// Exécuté par lvgl_task (lv_async_call), mutex LVGL tenu par sa boucle.
void snap_async(void *) {
  lv_draw_buf_t *b =
      lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
  if (!s_snap_waiting.exchange(false)) {
    if (b != nullptr) {
      lv_draw_buf_destroy(b); // l'attente a expiré : personne ne le lira
    }
    return;
  }
  s_snap.store(b);
  xSemaphoreGive(s_snap_done);
}

lv_draw_buf_t *take_snapshot() {
  s_snap.store(nullptr);
  s_snap_waiting.store(true);
  {
    bsp::Bsp::LvglLock lock(1000);
    if (!lock.held() ||
        lv_async_call(snap_async, nullptr) != LV_RESULT_OK) {
      s_snap_waiting.store(false);
      return nullptr;
    }
  }
  if (xSemaphoreTake(s_snap_done, pdMS_TO_TICKS(kSnapTimeoutMs)) == pdTRUE) {
    return s_snap.load();
  }
  if (!s_snap_waiting.exchange(false)) {
    // Le rappel a livré entre l'expiration et cette ligne : le prendre.
    xSemaphoreTake(s_snap_done, portMAX_DELAY);
    return s_snap.load();
  }
  return nullptr; // le rappel, s'il passe un jour, libérera son tampon
}

// L'écriture SD (~2 Mo, plusieurs secondes) se fait ici, sans le verrou LVGL,
// pour ne pas figer l'UI.
bool capture(const char *page) {
  lv_draw_buf_t *snap = take_snapshot();
  if (snap == nullptr) {
    ESP_LOGE(TAG, "%s : snapshot impossible (PSRAM ou tâche LVGL)", page);
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
  s_snap_done = xSemaphoreCreateBinary();
  if (s_wake == nullptr || s_snap_done == nullptr) {
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
