#include "sys_stats.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdio>

namespace sys_stats {

namespace {

// Échantillonneur unique + cache. Avant : chaque appelant (video_render_task,
// un timer ui_stats par écran, le timer stats météo) rentrait dans le calcul
// et se partageait les MÊMES prev_* statiques -> chacun mesurait le delta
// depuis l'appel du voisin, pas depuis sa propre fenêtre, et les pourcentages
// affichés étaient faux. En prime, uxTaskGetSystemState() parcourt toutes les
// tâches avec le scheduler suspendu : le payer 6 à 8 fois par seconde était
// pure perte. Un seul échantillon par fenêtre, les autres lisent le cache.
constexpr int64_t kMinSamplePeriodUs = 900000;

// Tag propre au dump par tâche : il reste distinct des logs du pipeline vidéo.
constexpr const char *kCpuTag = "CPU";

// Lu par la tâche de rendu vidéo, écrit depuis une autre tâche -> atomique.
std::atomic<bool> s_perf_diag{false};

std::atomic_flag s_sampling = ATOMIC_FLAG_INIT;
std::atomic<int> s_cached_a{0};
std::atomic<int> s_cached_b{0};
std::atomic<int64_t> s_last_sample_us{0};

// Coût de la mesure elle-même, cumulé sur la fenêtre puis remis à zéro à
// l'affichage. uxTaskGetSystemState ne se contente pas de lire des compteurs :
// prvListTasksWithinSingleList appelle vTaskGetInfo(..., pdTRUE, ...) pour
// CHAQUE tâche, ce qui balaie sa pile inutilisée octet par octet à la recherche
// du motif 0xa5 — et les piles de mjpeg et peer_loop sont en PSRAM. Le tout
// noyau verrouillé, donc c'est du temps où plus rien ne draine la socket.
// Les deux appelants (cette fonction et log_task_cpu) y contribuent.
//
// ⚠️ MESURÉ le 2026-07-31 en streaming à ~1030 Ko/s : probe = 3,4 ms/s, soit
// 0,34 %. Le diagnostic ne bride PAS le pipeline — inutile de re-soupçonner ni
// ce calcul ni le log LVGL_MUX (2 esp_timer_get_time() par verrou, et la tâche
// mjpeg ne prend jamais ce mutex). Les 60 % de mjpeg/0 sont dans lwip/esp_hosted.
std::atomic<uint32_t> s_probe_us{0};

} // namespace

CpuUsage cpu_usage_percent() {
  static TaskStatus_t *st = NULL;
  static UBaseType_t st_cap = 0;
  // Variables pour mémoriser les cycles précédents pour chaque cœur. Protégées
  // par s_sampling : un seul échantillonneur à la fois.
  static uint32_t prev_total = 0, prev_idle_a = 0, prev_idle_b = 0;

  const int64_t now_us = esp_timer_get_time();
  // Fenêtre pas encore écoulée, ou un autre appelant échantillonne déjà :
  // renvoyer la dernière valeur connue.
  if (now_us - s_last_sample_us.load(std::memory_order_relaxed) <
          kMinSamplePeriodUs ||
      s_sampling.test_and_set(std::memory_order_acquire)) {
    return {s_cached_a.load(std::memory_order_relaxed),
            s_cached_b.load(std::memory_order_relaxed)};
  }

  UBaseType_t n = uxTaskGetNumberOfTasks();
  if (n > st_cap) {
    if (st != nullptr) {
      heap_caps_free(st);
    }
    // +8 tâches de marge : évite de réallouer à chaque création de tâche
    // (sessions caméra/micro). Tableau en PSRAM (règle projet, ~100 o/tâche).
    st_cap = n + 8;
    st = (TaskStatus_t *)heap_caps_malloc(st_cap * sizeof(TaskStatus_t),
                                          MALLOC_CAP_SPIRAM);
    if (st == NULL) {
      st_cap = 0;
      s_sampling.clear(std::memory_order_release);
      return {0, 0};
    }
  }

  uint32_t total = 0;
  const int64_t probe0 = esp_timer_get_time();
  UBaseType_t got = uxTaskGetSystemState(st, st_cap, &total);
  s_probe_us.fetch_add((uint32_t)(esp_timer_get_time() - probe0),
                       std::memory_order_relaxed);

  uint32_t idle_a = 0;
  uint32_t idle_b = 0;

  // Comparaison par handle de la tâche idle de chaque cœur : plus fiable que
  // de matcher le nom ("IDLE0"/"IDLE1" varient selon la config FreeRTOS).
  TaskHandle_t idle_handle_a = xTaskGetIdleTaskHandleForCore(0);
  TaskHandle_t idle_handle_b = xTaskGetIdleTaskHandleForCore(1);

  for (UBaseType_t i = 0; i < got; i++) {
    if (st[i].xHandle == idle_handle_a) {
      idle_a = st[i].ulRunTimeCounter;
    } else if (st[i].xHandle == idle_handle_b) {
      idle_b = st[i].ulRunTimeCounter;
    }
  }

  // Soustractions non signées : correctes même après wrap du compteur µs.
  uint32_t d_total = total - prev_total;
  uint32_t d_idle_a = idle_a - prev_idle_a;
  uint32_t d_idle_b = idle_b - prev_idle_b;

  prev_total = total;
  prev_idle_a = idle_a;
  prev_idle_b = idle_b;

  if (d_total == 0) {
    s_sampling.clear(std::memory_order_release);
    return {s_cached_a.load(std::memory_order_relaxed),
            s_cached_b.load(std::memory_order_relaxed)};
  }

  // 1 cœur : temps CPU disponible sur la fenêtre = temps écoulé (d_total, sans
  // multiplier par 2)
  int pct_a = (int)(100 - (100ULL * d_idle_a) / (uint64_t)d_total);
  int pct_b = (int)(100 - (100ULL * d_idle_b) / (uint64_t)d_total);

  // Bornage : un compteur idle qui wrappe entre deux fenêtres peut sortir de
  // [0,100].
  if (pct_a < 0)
    pct_a = 0;
  else if (pct_a > 100)
    pct_a = 100;
  if (pct_b < 0)
    pct_b = 0;
  else if (pct_b > 100)
    pct_b = 100;

  s_cached_a.store(pct_a, std::memory_order_relaxed);
  s_cached_b.store(pct_b, std::memory_order_relaxed);
  s_last_sample_us.store(now_us, std::memory_order_relaxed);
  s_sampling.clear(std::memory_order_release);
  return {pct_a, pct_b};
}

namespace {

// Table de l'échantillon précédent, indexée par HANDLE : uxTaskGetSystemState
// ne rend que des compteurs cumulés depuis le boot, le delta est à notre
// charge. 48 entrées couvrent le pire cas (sessions caméra + micro ouvertes).
// Une table PAR APPELANT : deux appelants sur la même table mesureraient
// chacun le delta depuis l'appel de l'autre (cf. note en tête de fichier).
constexpr int kTracked = 48;
struct TaskSampler {
  TaskStatus_t *st = nullptr;
  UBaseType_t st_cap = 0;
  TaskHandle_t prev_h[kTracked] = {};
  uint32_t prev_rt[kTracked] = {};
  int prev_n = 0;
  uint32_t prev_total = 0;
  bool primed = false;
};

// min_core_pct : ne logge que si l'un des cœurs atteint ce seuil sur la
// fenêtre écoulée (0 = toujours). La table est mise à jour dans tous les cas,
// sinon la fenêtre suivante couvrirait deux périodes.
void sample_and_log(TaskSampler &s, int min_core_pct, const char *label) {
  constexpr int kTop = 7;    // tient dans une ligne UART
  constexpr int kMinPct = 3; // sous ce seuil, c'est du bruit d'échantillonnage
  TaskStatus_t *&st = s.st;
  UBaseType_t &st_cap = s.st_cap;
  TaskHandle_t(&prev_h)[kTracked] = s.prev_h;
  uint32_t(&prev_rt)[kTracked] = s.prev_rt;
  int &prev_n = s.prev_n;
  uint32_t &prev_total = s.prev_total;
  bool &primed = s.primed;

  UBaseType_t n = uxTaskGetNumberOfTasks();
  if (n > st_cap) {
    if (st != nullptr) {
      heap_caps_free(st);
    }
    st_cap = n + 8;
    st = static_cast<TaskStatus_t *>(
        heap_caps_malloc(st_cap * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM));
    if (st == nullptr) {
      st_cap = 0;
      return;
    }
  }

  uint32_t total = 0;
  const int64_t probe0 = esp_timer_get_time();
  UBaseType_t got = uxTaskGetSystemState(st, st_cap, &total);
  s_probe_us.fetch_add((uint32_t)(esp_timer_get_time() - probe0),
                       std::memory_order_relaxed);
  const uint32_t d_total = total - prev_total; // non signé : correct après wrap
  prev_total = total;

  // Pourcentages rapportés à UN cœur : en SMP IDF, pulTotalRunTime est le temps
  // écoulé, pas le temps écoulé x nb de cœurs (cf. cpu_usage_percent).
  int pct[kTracked];
  int idx[kTracked];
  int cnt = 0;
  TaskHandle_t idle0 = xTaskGetIdleTaskHandleForCore(0);
  TaskHandle_t idle1 = xTaskGetIdleTaskHandleForCore(1);
  // Charge PAR CŒUR. Additionner les pourcentages des tâches ne répond pas à
  // « quel cœur sature » : celles sans affinité (tcpip, sdio_read/proce/write)
  // rapportent tskNO_AFFINITY et leur temps n'est imputable à aucun cœur. Le
  // temps d'INACTIVITÉ, lui, l'est sans ambiguïté — chaque cœur a sa tâche IDLE.
  // D'où : charge du cœur N = 100 % - part de son IDLE. Seule mesure fiable ici,
  // et c'est elle qu'a confirmée le task watchdog sur IDLE0.
  uint32_t idle_d[2] = {0, 0};
  bool idle_known[2] = {false, false};
  for (UBaseType_t i = 0; i < got; i++) {
    uint32_t before = 0;
    bool known = false;
    for (int k = 0; k < prev_n; k++) {
      if (prev_h[k] == st[i].xHandle) {
        before = prev_rt[k];
        known = true;
        break;
      }
    }
    const uint32_t d = st[i].ulRunTimeCounter - before;
    if (st[i].xHandle == idle0 || st[i].xHandle == idle1) {
      const int c = (st[i].xHandle == idle0) ? 0 : 1;
      idle_d[c] = d;
      idle_known[c] = known;
      continue; // rapporté comme charge de cœur, pas comme ligne de tâche
    }
    // Tâche apparue depuis le dernier échantillon : son compteur cumulé n'est
    // pas un delta. La compter ferait apparaître un faux 100 %.
    if (!known || cnt >= kTracked) {
      continue;
    }
    int p = (d_total != 0) ? (int)((100ULL * d) / d_total) : 0;
    if (p < kMinPct) {
      continue;
    }
    pct[cnt] = (p > 100) ? 100 : p;
    idx[cnt] = (int)i;
    cnt++;
  }

  // Mémorise l'échantillon courant AVANT de sortir : sinon un retour anticipé
  // (première passe, rien au-dessus du seuil) laisserait la table périmée et le
  // delta suivant porterait sur deux fenêtres.
  prev_n = 0;
  for (UBaseType_t i = 0; i < got && prev_n < kTracked; i++) {
    prev_h[prev_n] = st[i].xHandle;
    prev_rt[prev_n] = st[i].ulRunTimeCounter;
    prev_n++;
  }
  if (!primed) {
    primed = true; // première passe : deltas non significatifs
    return;
  }
  if (min_core_pct > 0) {
    bool busy = false;
    for (int c = 0; c < 2; c++) {
      if (idle_known[c] && d_total != 0 &&
          100 - (int)((100ULL * idle_d[c]) / d_total) >= min_core_pct) {
        busy = true;
      }
    }
    if (!busy) {
      return;
    }
  }

  // Tri par sélection partielle sur kTop : ~35 tâches x 7 passes, 1x/s.
  char line[256];
  int j = 0;

  // Charge par cœur en tête de ligne : c'est le chiffre qui dit lequel sature,
  // et le seul que les tâches sans affinité ne faussent pas.
  for (int c = 0; c < 2; c++) {
    int load = -1;
    if (idle_known[c] && d_total != 0) {
      load = 100 - (int)((100ULL * idle_d[c]) / d_total);
      load = (load < 0) ? 0 : ((load > 100) ? 100 : load);
    }
    const int w = (load < 0)
                      ? snprintf(line + j, sizeof(line) - (size_t)j, " C%d=?", c)
                      : snprintf(line + j, sizeof(line) - (size_t)j, " C%d=%d%%",
                                 c, load);
    if (w < 0 || (size_t)w >= sizeof(line) - (size_t)j) {
      break;
    }
    j += w;
  }
  {
    const int w = snprintf(line + j, sizeof(line) - (size_t)j, " |");
    if (w > 0 && (size_t)w < sizeof(line) - (size_t)j) {
      j += w;
    }
  }

  for (int slot = 0; slot < kTop && slot < cnt; slot++) {
    int best = slot;
    for (int k = slot + 1; k < cnt; k++) {
      if (pct[k] > pct[best]) {
        best = k;
      }
    }
    int tmp = pct[slot];
    pct[slot] = pct[best];
    pct[best] = tmp;
    tmp = idx[slot];
    idx[slot] = idx[best];
    idx[best] = tmp;

    char core = '*'; // '*' = tskNO_AFFINITY, la tâche flotte entre les cœurs
#if defined(configTASKLIST_INCLUDE_COREID) && (configTASKLIST_INCLUDE_COREID == 1)
    if (st[idx[slot]].xCoreID == 0) {
      core = '0';
    } else if (st[idx[slot]].xCoreID == 1) {
      core = '1';
    }
#endif
    int w = snprintf(line + j, sizeof(line) - (size_t)j, " %.10s/%c=%d%%",
                     st[idx[slot]].pcTaskName, core, pct[slot]);
    // snprintf rend la longueur VOULUE : ne jamais avancer j au-delà du buffer.
    if (w < 0 || (size_t)w >= sizeof(line) - (size_t)j) {
      break;
    }
    j += w;
  }
  // Pas de retour anticipé quand aucune tâche ne passe le seuil : la charge par
  // cœur est justement l'information qu'on veut voir dans ce cas-là.
  // SRAM interne libre : c'est elle que consomment les pbufs lwip
  // (CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP est off), donc la ressource que
  // l'élargissement de la fenêtre TCP met sous tension. La surveiller ici évite
  // de découvrir la pénurie par un échec d'allocation WebRTC.
  const size_t sram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  // `probe=` : ce que coûte la mesure, noyau verrouillé, sur la fenêtre écoulée.
  // À lire comme une fraction de seconde (10000 us = 1 % du CPU, et surtout 1 %
  // du temps pendant lequel la socket MJPEG n'est drainée par personne).
  const uint32_t probe = s_probe_us.exchange(0, std::memory_order_relaxed);
  ESP_LOGI(kCpuTag, "%s%s | SRAM interne libre %u Ko | probe=%uus", label,
           line, (unsigned)(sram_free / 1024), (unsigned)probe);
}

} // namespace

void log_task_cpu() {
  static TaskSampler s;
  // La garde couvre l'ÉCHANTILLONNAGE, pas seulement l'écriture :
  // uxTaskGetSystemState() parcourt toutes les tâches ordonnanceur suspendu,
  // c'est lui le coût. primed remis à false pour que la table reparte propre à
  // la réactivation, sinon le premier delta couvrirait toute la période d'arrêt.
  if (!s_perf_diag.load(std::memory_order_relaxed)) {
    s.primed = false;
    return;
  }
  sample_and_log(s, 0, "");
}

void log_busy_tasks(int min_core_pct) {
  static TaskSampler s;
  sample_and_log(s, min_core_pct, " [hors session]");
}

void set_perf_diag(bool on) {
  s_perf_diag.store(on, std::memory_order_relaxed);
}

bool perf_diag() { return s_perf_diag.load(std::memory_order_relaxed); }

int psram_used_percent() {
  size_t total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  if (total == 0) {
    return 0; // pas de PSRAM déclarée : rien à rapporter
  }
  size_t free_b = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  return (int)(100 - (100ULL * free_b) / total);
}

} // namespace sys_stats