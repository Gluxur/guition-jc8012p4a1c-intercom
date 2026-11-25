#include "sys/crumb.h"

#include <atomic>
#include <cstddef>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

namespace crumb {
namespace {

constexpr const char *TAG = "crumb";
// Changé à chaque ajout d'Id : la disposition de Store en dépend, et un ancien
// contenu relu avec la nouvelle disposition mélangerait les étapes.
constexpr uint32_t kMagic = 0xC0B1D2E4;
constexpr size_t kN = static_cast<size_t>(Id::kCount);

// Indexés par Id ; libellés de log, pas des textes d'interface.
constexpr const char *kNames[] = {
    "gate_press",        "gate_queued",        "api_action_send",
    "api_action_sent",   "gate_done",          "band_compose_begin",
    "band_compose_end",  "band_blit_begin",    "band_blit_end",
    "jpeg_decode_begin", "jpeg_decode_end",    "present_begin",
    "present_end",       "presence_cam_stop",  "presence_capture_begin",
    "presence_capture_end", "presence_write_begin", "presence_write_end",
    "sdlog_write_begin", "sdlog_write_end",    "band_snap_done",
    "band_msync_begin",  "cam_stop",           "cam_history_begin",
    "cam_history_end",   "video_stop_begin",   "video_stop_end",
    "cam_peer_close",    "cam_teardown_end",   "mic_stop",
    "mic_send_joined",   "mic_peer_close",     "mic_teardown_end",
    "aec_begin",         "aec_end",            "aec_stop",
    "mic_read_begin",    "mic_read_end",       "mic_send_begin",
    "mic_send_end",
};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == kN,
              "un libellé par crumb::Id");

// Mémoire LP : non remise à zéro par un reset logiciel ni par un watchdog,
// seulement à la mise sous tension (magic alors invalide). Accès 32 bits
// alignés uniquement : pas d'atomiques RISC-V sur ce bus.
struct Store {
  uint32_t magic;
  uint32_t t_ms[kN];  // 0 = jamais vue ; sinon ms depuis le boot, +1
  uint32_t arg[kN];
  uint32_t core[kN];
};
RTC_NOINIT_ATTR Store s_store;

// Posé une fois par report_previous() (app_main), lu par toutes les tâches.
std::atomic<bool> s_ready{false};

} // namespace

void mark(Id id, uint32_t arg) {
  const size_t i = static_cast<size_t>(id);
  if (!s_ready.load(std::memory_order_relaxed) || i >= kN) {
    return;
  }
  s_store.arg[i] = arg;
  s_store.core[i] = static_cast<uint32_t>(esp_cpu_get_core_id());
  // Horodatage en dernier : une étape n'apparaît qu'avec son arg déjà écrit.
  s_store.t_ms[i] = static_cast<uint32_t>(esp_timer_get_time() / 1000) + 1;
}

void report_previous() {
  const esp_reset_reason_t r = esp_reset_reason();
  const bool unexpected = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
                          r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
                          r == ESP_RST_BROWNOUT;
  if (s_store.magic == kMagic && unexpected) {
    uint32_t last = 0;
    for (size_t i = 0; i < kN; i++) {
      last = s_store.t_ms[i] > last ? s_store.t_ms[i] : last;
    }
    ESP_LOGW(TAG, "dernières étapes avant le reset (t=0 : dernier marqueur) :");
    // Du plus récent au plus ancien ; kN est petit, tri par sélection.
    bool shown[kN] = {};
    for (size_t n = 0; n < kN; n++) {
      size_t best = kN;
      for (size_t i = 0; i < kN; i++) {
        if (!shown[i] && s_store.t_ms[i] != 0 &&
            (best == kN || s_store.t_ms[i] > s_store.t_ms[best])) {
          best = i;
        }
      }
      if (best == kN) {
        break;
      }
      shown[best] = true;
      ESP_LOGW(TAG, "  t=-%lu ms  %-22s core %lu arg %lu",
               static_cast<unsigned long>(last - s_store.t_ms[best]),
               kNames[best], static_cast<unsigned long>(s_store.core[best]),
               static_cast<unsigned long>(s_store.arg[best]));
    }
  } else if (unexpected) {
    ESP_LOGW(TAG, "reset inattendu mais aucun marqueur conservé");
  }
  for (size_t i = 0; i < kN; i++) {
    s_store.t_ms[i] = 0;
    s_store.arg[i] = 0;
    s_store.core[i] = 0;
  }
  s_store.magic = kMagic;
  s_ready.store(true, std::memory_order_release);
}

} // namespace crumb
