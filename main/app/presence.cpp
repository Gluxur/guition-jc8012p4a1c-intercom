#include "presence.h"

#include <atomic>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h" // xTaskCreatePinnedToCoreWithCaps
#include "freertos/task.h"

#include "app/camera_session.h"
#include "app/motion_detect.h"
#include "app_config.h"
#include "bsp.h"
#include "sys/usb_msc.h"

namespace presence {

namespace {

constexpr const char *TAG = "presence";

// ~2 analyses/s : un pas traverse une cellule (~30 px à 3-4 m) en bien plus
// d'une demi-seconde, et chaque image lue coûte un passage PSRAM.
constexpr uint32_t kPeriodMs = 500;
constexpr uint32_t kCaptureTimeoutMs = 1000; // 5 trames à 5 img/s
constexpr int kRowStep = 4;

// Seuils de détection — PROVISOIRES, à régler sur les max_delta du log de
// diagnostic (1x/min) une fois la caméra vue en conditions réelles.
constexpr int kCellThreshold = 12; // niveaux 8 bits, après retrait du global
constexpr int kMinCells = 3;       // de 960 : ignore un pixel chaud isolé

// Après le dernier mouvement, l'état reste « présent » ce temps-là. Court
// exprès : le délai d'extinction (1 min) est celui de l'automatisation HA,
// qui a sa propre temporisation — doubler ici ne ferait que l'allonger.
constexpr int64_t kHoldUs = 10LL * 1000 * 1000;

// Exposition : consigne et niveau de noir en 8 bits (0x4003 = 64 en RAW10 ->
// 16 en 8 bits). Valeur de départ = celle de la table Guition (0x046C lignes,
// gain x8), connue pour donner une image.
constexpr uint8_t kAeTarget = 70;
constexpr uint8_t kBlackLevel = 16;
constexpr uint32_t kAeMinLines = 4;
constexpr uint32_t kAeMaxGainX16 = 248;
constexpr motion::Exposure kAeStart = {0x046C, 128};

constexpr int64_t kStatsPeriodUs = 60LL * 1000 * 1000;

Hooks s_hooks{};
std::atomic<bool> s_present{false};
std::atomic<bool> s_snapshot_req{false};

void set_present(bool p) {
  if (s_present.exchange(p) != p && s_hooks.on_change) {
    s_hooks.on_change();
  }
}

// Vignette : un pixel sur 4 dans chaque axe, 8 bits de poids fort. POSIX
// open/write et non FILE* : cf. docs/hardware-notes.md (picolibc déborde d'un
// octet du tampon FILE) — inutile de rouvrir ce risque pour un fichier jetable.
void write_snapshot(const uint8_t *frame) {
  if (bsp::Bsp::instance().sd_state() != bsp::SdState::kMounted ||
      usb_msc::status() == usb_msc::Status::kActive) {
    ESP_LOGW(TAG, "cliché ignoré : carte SD indisponible");
    return;
  }
  constexpr int kTw = bsp::Bsp::kCamWidth / 4;
  constexpr int kTh = bsp::Bsp::kCamHeight / 4;
  constexpr int kStride = bsp::Bsp::kCamWidth * 10 / 8;
  uint8_t *img = static_cast<uint8_t *>(
      heap_caps_malloc(static_cast<size_t>(kTw) * kTh, MALLOC_CAP_SPIRAM));
  if (img == nullptr) {
    ESP_LOGE(TAG, "cliché : pas de PSRAM");
    return;
  }
  for (int y = 0; y < kTh; y++) {
    const uint8_t *row = frame + static_cast<size_t>(y) * 4 * kStride;
    for (int x = 0; x < kTw; x++) {
      img[y * kTw + x] = row[x * 5]; // groupe de 4 px : 1er pixel
    }
  }
  const int fd = open(cfg::sd::kPresenceSnapshot, O_WRONLY | O_CREAT | O_TRUNC,
                      0644);
  if (fd < 0) {
    ESP_LOGE(TAG, "cliché : ouverture impossible");
    heap_caps_free(img);
    return;
  }
  char hdr[32];
  const int n = snprintf(hdr, sizeof hdr, "P5\n%d %d\n255\n", kTw, kTh);
  bool ok = n > 0 && write(fd, hdr, n) == n;
  ok = ok && write(fd, img, static_cast<size_t>(kTw) * kTh) ==
                 static_cast<ssize_t>(kTw) * kTh;
  close(fd);
  heap_caps_free(img);
  ESP_LOGI(TAG, "cliché %s : %s", cfg::sd::kPresenceSnapshot,
           ok ? "écrit" : "échec d'écriture");
}

void presence_task(void *) {
  bsp::Bsp &bsp = bsp::Bsp::instance();
  uint8_t *frame = nullptr;
  // Grilles statiques : 2 x ~1 Ko, hors de la pile.
  static motion::Grid prev;
  static motion::Grid cur;
  bool have_prev = false;
  motion::Exposure exp = kAeStart;
  int64_t last_motion_us = 0;
  int64_t last_stats_us = esp_timer_get_time();
  int stat_frames = 0, stat_max_delta = 0, stat_max_changed = 0;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kPeriodMs));

    // En session : caméra coupée, état forcé. Le CSI ne doit rien prendre
    // au pipeline vidéo (PSRAM, IRQ) ni au DTLS (SRAM interne).
    if (camera_session::active()) {
      if (bsp.camera_running()) {
        (void)bsp.camera_stop();
        heap_caps_free(frame);
        frame = nullptr;
        have_prev = false;
      }
      set_present(true);
      last_motion_us = esp_timer_get_time();
      continue;
    }

    if (!bsp.camera_running()) {
      if (frame == nullptr) {
        frame = static_cast<uint8_t *>(heap_caps_aligned_alloc(
            64, bsp::Bsp::kCamFrameBytes, MALLOC_CAP_SPIRAM));
        if (frame == nullptr) {
          ESP_LOGE(TAG, "pas de PSRAM pour l'image");
          vTaskDelay(pdMS_TO_TICKS(30000));
          continue;
        }
      }
      const esp_err_t err = bsp.camera_start();
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "démarrage caméra : %s, nouvel essai dans 60 s",
                 esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(60000));
        continue;
      }
      (void)bsp.camera_set_exposure(exp.lines, exp.gain_x16);
      have_prev = false;
    }

    const esp_err_t err =
        bsp.camera_capture(frame, bsp::Bsp::kCamFrameBytes, kCaptureTimeoutMs);
    if (err != ESP_OK) {
      // Une ligne d'état complète toutes les 20 expirations (~30 s) : assez
      // pour diagnostiquer, sans inonder le journal SD.
      static int s_timeouts = 0;
      if (s_timeouts++ % 20 == 0) {
        ESP_LOGW(TAG, "capture : %s", esp_err_to_name(err));
        bsp.camera_log_link_state();
      }
      continue;
    }

    if (s_snapshot_req.exchange(false)) {
      write_snapshot(frame);
    }

    motion::reduce_raw10(frame, bsp::Bsp::kCamWidth, bsp::Bsp::kCamHeight,
                         kRowStep, cur);

    const int64_t now = esp_timer_get_time();
    if (have_prev) {
      const motion::Diff d = motion::compare(prev, cur, kCellThreshold);
      stat_frames++;
      if (d.max_delta > stat_max_delta) {
        stat_max_delta = d.max_delta;
      }
      if (d.changed > stat_max_changed) {
        stat_max_changed = d.changed;
      }
      if (d.changed >= kMinCells) {
        last_motion_us = now;
        set_present(true);
      }
    }
    if (s_present.load() && now - last_motion_us > kHoldUs) {
      set_present(false);
    }

    // AE APRÈS la comparaison : l'image suivante, prise à une autre
    // exposition, ne doit pas être comparée à celle-ci (have_prev = false).
    const motion::Exposure next = motion::next_exposure(
        exp, cur.mean, kAeTarget, kBlackLevel,
        bsp::Bsp::camera_max_exposure_lines(), kAeMinLines, kAeMaxGainX16);
    if (next.lines != exp.lines || next.gain_x16 != exp.gain_x16) {
      exp = next;
      (void)bsp.camera_set_exposure(exp.lines, exp.gain_x16);
      have_prev = false;
    } else {
      prev = cur;
      have_prev = true;
    }

    // Diagnostic 1x/min : de quoi régler les seuils sur mesure.
    if (now - last_stats_us >= kStatsPeriodUs) {
      ESP_LOGI(TAG,
               "moy=%u pose=%u l gain=%u/16 | %d comparaisons, max_delta=%d "
               "max_cellules=%d | %s",
               cur.mean, (unsigned)exp.lines, (unsigned)exp.gain_x16,
               stat_frames, stat_max_delta, stat_max_changed,
               s_present.load() ? "présent" : "absent");
      last_stats_us = now;
      stat_frames = stat_max_delta = stat_max_changed = 0;
    }
  }
}

} // namespace

void init(const Hooks &hooks) {
  s_hooks = hooks;
  // Cœur 0, priorité 2 : la caméra ne tourne que hors session, quand le cœur
  // du drain MJPEG est au repos. Pile en PSRAM (règle projet) ; 6 Ko pour
  // snprintf/ESP_LOGI et l'IO SD du cliché (FATFS veut de la marge, cf.
  // mémoire « cam_stop stack overflow »).
  if (xTaskCreatePinnedToCoreWithCaps(presence_task, "presence", 6144, nullptr,
                                      2, nullptr, 0,
                                      MALLOC_CAP_SPIRAM) != pdPASS) {
    ESP_LOGE(TAG, "création de la tâche impossible");
  }
}

bool present() { return s_present.load(); }

void request_snapshot() { s_snapshot_req.store(true); }

} // namespace presence
