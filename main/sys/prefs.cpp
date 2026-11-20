#include "prefs.h"

#include "esp_log.h"
#include "nvs.h"

#include <atomic>
#include <cstdio>

namespace prefs {

namespace {

constexpr char TAG[] = "prefs";
constexpr char kNamespace[] = "ui_prefs";
constexpr char kDndKey[] = "dnd";
constexpr char kBootCountKey[] = "boot_count";
constexpr char kShowStatsKey[] = "stats";
constexpr char kRingVolKey[] = "ring_vol";
constexpr char kSdLogKey[] = "sdlog";
constexpr char kRingFileKey[] = "ring_file";
constexpr char kFileSrvKey[] = "filesrv";
constexpr char kEchoKey[] = "echo";
constexpr char kThemeKey[] = "theme";

// Écrit par la tâche LVGL (page Configuration), lu par la tâche esp-mqtt
// (déclencheur de sonnerie) : atomique, jamais volatile.
std::atomic<bool> s_dnd{false};

// Écrit par la tâche LVGL, lu par les tâches de rendu (mjpeg_rnd) et par les
// timers d'overlay : atomique.
std::atomic<bool> s_show_stats{false};

// Écrit par la tâche LVGL (page Configuration), lu au boot par app_main :
// atomique, par cohérence avec les autres préférences.
std::atomic<uint8_t> s_ring_volume{100};

// Écrit par la tâche LVGL (page Configuration), lu par sd_log::start() puis par
// le sink de log, qui s'exécute dans le contexte de N'IMPORTE quelle tâche
// appelant ESP_LOGx : atomique, obligatoirement.
std::atomic<bool> s_sd_log{true};

// Écrit depuis la tâche LVGL (page Configuration) ET la tâche esp-mqtt (entité
// Home Assistant) : atomique, obligatoirement.
std::atomic<bool> s_file_server{false};

// Écrit par la tâche LVGL, lu par mic_send au décroché : atomique.
std::atomic<EchoCancel> s_echo{EchoCancel::kLowCost};

// Écrit par la tâche LVGL, lu par ui::create_screens() au boot : atomique, par
// cohérence avec les autres préférences.
std::atomic<UiTheme> s_theme{UiTheme::kVice};

// Pas d'atomique, contrairement aux autres préférences : une chaîne ne se lit
// pas d'un mot. Écrivaine unique : la tâche LVGL (page Configuration).
// Lectrices : app_main (avant que l'UI existe) et ringtone_sd::load() depuis la
// tâche disque de ui_sdlog, à la sortie du mode clé USB. Les deux gestes
// utilisateur — choisir un fichier, quitter le mode clé USB — vivent sur des
// écrans différents d'une dalle tactile unique : ils ne peuvent pas se
// chevaucher. Ajouter un verrou ici demanderait de le tenir aussi dans le
// chemin de chargement, pour une course que l'IHM rend impossible.
char s_ring_file[kRingFileMax] = "ring.wav";

// Écrit une fois depuis app_main, lu par la tâche d'écriture SD : atomique,
// par cohérence avec s_dnd.
std::atomic<uint32_t> s_boot_count{0};

} // namespace

void init() {
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return; // namespace jamais écrit : défaut (DND inactif)
  }
  uint8_t val = 0;
  if (nvs_get_u8(handle, kDndKey, &val) == ESP_OK) {
    s_dnd.store(val != 0);
  }
  if (nvs_get_u8(handle, kShowStatsKey, &val) == ESP_OK) {
    s_show_stats.store(val != 0);
  }
  if (nvs_get_u8(handle, kRingVolKey, &val) == ESP_OK) {
    s_ring_volume.store(val > 100 ? 100 : val);
  }
  if (nvs_get_u8(handle, kSdLogKey, &val) == ESP_OK) {
    s_sd_log.store(val != 0);
  }
  if (nvs_get_u8(handle, kFileSrvKey, &val) == ESP_OK) {
    s_file_server.store(val != 0);
  }
  if (nvs_get_u8(handle, kEchoKey, &val) == ESP_OK &&
      val <= static_cast<uint8_t>(EchoCancel::kHighPerf)) {
    s_echo.store(static_cast<EchoCancel>(val));
  }
  if (nvs_get_u8(handle, kThemeKey, &val) == ESP_OK &&
      val <= static_cast<uint8_t>(UiTheme::kVice)) {
    s_theme.store(static_cast<UiTheme>(val));
  }
  size_t len = sizeof(s_ring_file);
  if (nvs_get_str(handle, kRingFileKey, s_ring_file, &len) != ESP_OK) {
    // Clé absente : on garde "ring.wav", le nom historique. nvs_get_str laisse
    // le tampon indéterminé en cas d'échec, d'où la restauration explicite.
    snprintf(s_ring_file, sizeof(s_ring_file), "ring.wav");
  }
  s_ring_file[sizeof(s_ring_file) - 1] = '\0';
  uint32_t boots = 0;
  if (nvs_get_u32(handle, kBootCountKey, &boots) == ESP_OK) {
    s_boot_count.store(boots);
  }
  nvs_close(handle);
  ESP_LOGI(TAG,
           "ne pas déranger : %s | stats : %s | volume sonnerie : %u %% | "
           "journal SD : %s | sonnerie : %s | serveur fichiers : %s | "
           "écho : %u | thème : %u",
           s_dnd.load() ? "actif" : "inactif",
           s_show_stats.load() ? "affichées" : "masquées",
           (unsigned)s_ring_volume.load(),
           s_sd_log.load() ? "actif" : "inactif",
           s_ring_file[0] ? s_ring_file : "(interne)",
           s_file_server.load() ? "actif" : "inactif",
           (unsigned)s_echo.load(), (unsigned)s_theme.load());
}

bool dnd() { return s_dnd.load(); }

void set_dnd(bool on) {
  s_dnd.store(on);
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open %s en écriture : réglage non persisté", kNamespace);
    return;
  }
  nvs_set_u8(handle, kDndKey, on ? 1 : 0);
  nvs_commit(handle);
  nvs_close(handle);
}

bool show_stats() { return s_show_stats.load(); }

void set_show_stats(bool on) {
  s_show_stats.store(on);
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open %s en écriture : réglage non persisté", kNamespace);
    return;
  }
  nvs_set_u8(handle, kShowStatsKey, on ? 1 : 0);
  nvs_commit(handle);
  nvs_close(handle);
}

uint8_t ring_volume() { return s_ring_volume.load(); }

void set_ring_volume(uint8_t percent) {
  if (percent > 100) {
    percent = 100;
  }
  s_ring_volume.store(percent);
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open %s en écriture : réglage non persisté", kNamespace);
    return;
  }
  nvs_set_u8(handle, kRingVolKey, percent);
  nvs_commit(handle);
  nvs_close(handle);
}

const char *ring_file() { return s_ring_file; }

void set_ring_file(const char *name) {
  snprintf(s_ring_file, sizeof(s_ring_file), "%s", name ? name : "");
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open %s en écriture : réglage non persisté", kNamespace);
    return;
  }
  nvs_set_str(handle, kRingFileKey, s_ring_file);
  nvs_commit(handle);
  nvs_close(handle);
}

bool sd_log_enabled() { return s_sd_log.load(); }

void set_sd_log_enabled(bool on) {
  s_sd_log.store(on);
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open %s en écriture : réglage non persisté", kNamespace);
    return;
  }
  nvs_set_u8(handle, kSdLogKey, on ? 1 : 0);
  nvs_commit(handle);
  nvs_close(handle);
}

bool file_server() { return s_file_server.load(); }

void set_file_server(bool on) {
  s_file_server.store(on);
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open %s en écriture : réglage non persisté", kNamespace);
    return;
  }
  nvs_set_u8(handle, kFileSrvKey, on ? 1 : 0);
  nvs_commit(handle);
  nvs_close(handle);
}

EchoCancel echo_cancel() { return s_echo.load(); }

void set_echo_cancel(EchoCancel mode) {
  s_echo.store(mode);
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open %s en écriture : réglage non persisté", kNamespace);
    return;
  }
  nvs_set_u8(handle, kEchoKey, static_cast<uint8_t>(mode));
  nvs_commit(handle);
  nvs_close(handle);
}

uint32_t boot_count() { return s_boot_count.load(); }

void bump_boot_count() {
  const uint32_t next = s_boot_count.load() + 1;
  s_boot_count.store(next);
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    // Échec sans conséquence : le compteur reste juste pour cette session.
    ESP_LOGE(TAG, "nvs_open %s en écriture : compteur de boot non persisté",
             kNamespace);
    return;
  }
  nvs_set_u32(handle, kBootCountKey, next);
  nvs_commit(handle);
  nvs_close(handle);
}

UiTheme ui_theme() { return s_theme.load(); }

void set_ui_theme(UiTheme theme) {
  s_theme.store(theme);
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open %s en écriture : réglage non persisté", kNamespace);
    return;
  }
  nvs_set_u8(handle, kThemeKey, static_cast<uint8_t>(theme));
  nvs_commit(handle);
  nvs_close(handle);
}

} // namespace prefs
