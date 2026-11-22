#include "ha_api.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app/call_history.h"
#include "app/camera_session.h"
#include "app/gate.h"
#include "app/ha_entity_id.h"
#include "app/presence.h"
#include "app_config.h"
#include "bsp.h"
#include "media/ringtone.h"
#include "net/esphome_api.h"
#include "net/file_server.h"
#include "net/wifi_net.h"
#include "sys/ota_ctrl.h"
#include "sys/prefs.h"
#include "sys/sd_log.h"
#include "sys/settings.h"
#include "sys/sys_stats.h"
#include "sys/usb_msc.h"
#include "ui/ui.h"
#include "ui/ui_camera_v2.h"

namespace ha_api {
namespace {

constexpr const char *TAG = "ha_api";

using esphome_api::Category;
using esphome_api::Entity;
using esphome_api::Kind;

Hooks s_hooks = {};

int clamp_pct(float v) {
  // HA sliders send floats; out-of-range values are ignored by the callers
  // (the echo then republishes the real value).
  return (int)lroundf(v);
}

// --- Screen (light) ------------------------------------------------------------

// On/off comes from the BSP (display_on), NOT backlight(), which returns the
// REMEMBERED setting: switching off leaves it intact on purpose. During a call
// the panel is pushed to 100 %, but that is not what the user chose.
bool g_screen_on() { return bsp::Bsp::instance().display_on(); }
float g_screen_bri() { return bsp::Bsp::instance().backlight() / 100.0f; }

void s_screen(int state, float bri) {
  bsp::Bsp &b = bsp::Bsp::instance();
  if (!std::isnan(bri)) {
    const int v = (int)lroundf(bri * 100.0f);
    // 0 must NOT go through set_backlight: it persists to NVS and the board
    // would reboot to a black screen with no way to fix it from the screen
    // (cf. Bsp::backlight_off). Switching off is a transient state.
    if (v <= 0) {
      b.backlight_off();
      return;
    }
    b.set_backlight((uint8_t)(v > 100 ? 100 : v));
    return;
  }
  if (state == 0) {
    b.backlight_off();
  } else if (state == 1) {
    b.restore_backlight(); // back to the remembered brightness
  }
}

// --- Switches -------------------------------------------------------------------

bool g_dnd() { return prefs::dnd(); }
void s_dnd(bool on) { prefs::set_dnd(on); }

bool g_stats() { return prefs::show_stats(); }
void s_stats(bool on) { prefs::set_show_stats(on); }

bool g_sdlog() { return sd_log::enabled(); }
// Same "apply then persist" pair as the Settings page: prefs drives nothing.
void s_sdlog(bool on) {
  sd_log::set_enabled(on);
  prefs::set_sd_log_enabled(on);
}

bool g_filesrv() { return file_server::running(); }
void s_filesrv(bool on) {
  if (on) {
    if (!file_server::start()) {
      return; // the echo republishes the real state: the entity won't lie
    }
  } else {
    file_server::stop();
  }
  prefs::set_file_server(on);
}

bool g_call() { return camera_session::active(); }

// Opening "by hand", like the Camera button of the home screen: no indoor
// ringtone, no street-side ding-dong, nothing in the call log. That is what
// allows measuring remotely (and at night) without waking the street or
// faking missed calls — the "ring" button does all of that.
void s_camera(bool on) {
  if (on) {
    if (camera_session::active() ||
        usb_msc::status() == usb_msc::Status::kActive) {
      return;
    }
    ui_camera_v2::open_screen();
  } else if (camera_session::active()) {
    camera_session::stop();
  }
}

// --- Numbers -------------------------------------------------------------------

float g_ring_volume() { return (float)prefs::ring_volume(); }
void s_ring_volume(float f) {
  const int v = clamp_pct(f);
  if (v < 0 || v > 100) {
    return;
  }
  prefs::set_ring_volume((uint8_t)v);
  ringtone::set_volume((uint8_t)v);
}

float g_speaker_volume() { return (float)bsp::Bsp::instance().speaker_volume(); }
void s_speaker_volume(float f) {
  const int v = clamp_pct(f);
  if (v < 0 || v > 100) {
    return;
  }
  bsp::Bsp::instance().set_speaker_volume((uint8_t)v);
}

float g_mic_gain() { return (float)bsp::Bsp::instance().mic_gain(); }
// Remote-adjustable: too much gain clips the ADC on word attacks, and G.711
// turns clipping into crackle on the street side — a setting tuned between
// two rings, not from the tablet.
void s_mic_gain(float f) {
  const int v = clamp_pct(f);
  if (v < 0 || v > 100) {
    return;
  }
  bsp::Bsp::instance().set_mic_gain((uint8_t)v);
}

// --- Selects -------------------------------------------------------------------

// Labels in prefs::EchoCancel order: the index IS the setting, as in the
// dropdown of the Settings page.
const char *const kEchoLabels[] = {"Desactivee", "Economique",
                                   "Haute performance"};

void g_echo(char *o, size_t c) {
  snprintf(o, c, "%s", kEchoLabels[static_cast<int>(prefs::echo_cancel())]);
}
void s_echo(const char *v) {
  for (size_t i = 0; i < sizeof(kEchoLabels) / sizeof(kEchoLabels[0]); i++) {
    if (strcmp(v, kEchoLabels[i]) == 0) {
      prefs::set_echo_cancel(static_cast<prefs::EchoCancel>(i));
      return;
    }
  }
}

const char *const kPages[] = {"accueil",       "meteo", "agenda", "appels",
                              "configuration", "autre"};

void g_page(char *o, size_t c) { snprintf(o, c, "%s", ui::current_page()); }

// Remote navigation (load measurements per screen, driving from HA). Refused
// during a camera session (direct_mode: the video screen owns the
// framebuffer, an LVGL page loaded underneath would stay invisible) and in
// USB mode (its dedicated screen carries the way out of the mode).
void s_page(const char *name) {
  if (camera_session::active() ||
      usb_msc::status() == usb_msc::Status::kActive) {
    return;
  }
  if (!ui::open_page(name)) {
    ESP_LOGW(TAG, "page « %s » : inconnue ou LVGL indisponible", name);
    return;
  }
  // The page opens in lvgl_task: give it a turn before the echo that follows,
  // or HA would receive the old page.
  vTaskDelay(pdMS_TO_TICKS(300));
}

// --- Buttons -------------------------------------------------------------------

void p_ring() {
  if (s_hooks.ring != nullptr) {
    s_hooks.ring();
  }
}

void p_reboot() {
  ESP_LOGI(TAG, "redémarrage demandé par Home Assistant");
  sd_log::flush_now();
  esp_restart();
}

void p_ack_missed() {
  call_history::acknowledge_missed();
  publish_now("missed_calls");
}

void p_presence_snapshot() { presence::request_snapshot(); }

void p_ota_sd() { ota_ctrl::request_file(cfg::kOtaSdDefaultPath); }

// The image Home Assistant serves from /config/www/fw.bin. Only the
// configured host can serve firmware: the URL is built here, never received.
// Firmware update entity. The image HA offers is whatever sits in
// /config/www/fw.bin: its version is read from the image header (a 144-byte
// Range request), no manifest to keep in sync with the binary.
void g_update(esphome_api::UpdateState *u) {
  const esp_app_desc_t *d = esp_app_get_description();
  snprintf(u->current, sizeof u->current, "%s", d ? d->version : "unknown");
  snprintf(u->title, sizeof u->title, "Visiophone firmware");
  u->in_progress = ota_ctrl::in_progress();
  const int pct = ota_ctrl::progress();
  u->progress = pct >= 0 ? (float)pct : -1.0f;
  ota_ctrl::ImageInfo img = {};
  if (ota_ctrl::latest(&img) && img.version[0] != '\0') {
    snprintf(u->latest, sizeof u->latest, "%s", img.version);
    snprintf(u->summary, sizeof u->summary, "/local/fw.bin: %s, built %s",
             img.version, img.date);
  }
}

void c_update(bool install) {
  if (install) {
    ota_ctrl::request(settings::ota_firmware_url());
  } else {
    ota_ctrl::request_check(settings::ota_firmware_url());
  }
}

// Each (re)connection of HA: look for a newer image right away; ota_ctrl then
// re-checks every 6 h on its own.
void on_ha_ready() { ota_ctrl::request_check(settings::ota_firmware_url()); }

// --- Sensors -------------------------------------------------------------------

float g_rssi() {
  const int r = wifi_net::wifi_rssi();
  return r == 0 ? NAN : (float)r;
}

void g_ip(char *o, size_t c) {
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip = {};
  if (netif == nullptr || esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
    snprintf(o, c, "0.0.0.0");
    return;
  }
  snprintf(o, c, IPSTR, IP2STR(&ip.ip));
}

float g_uptime() { return (float)(esp_timer_get_time() / 1000000); }
float g_psram() { return (float)sys_stats::psram_used_percent(); }
float g_sram_free() {
  return (float)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}
float g_cpu0() { return (float)sys_stats::cpu_usage_percent().core_a; }
float g_cpu1() { return (float)sys_stats::cpu_usage_percent().core_b; }

// sd_capacity() reads the card: this entity has sd_touching and is never
// polled during a camera session.
float g_sd_free() {
  uint64_t total = 0, freeb = 0;
  if (!bsp::Bsp::instance().sd_capacity(&total, &freeb)) {
    return NAN;
  }
  return (float)(freeb / (1024 * 1024));
}

float g_missed() { return (float)call_history::missed_count(); }

void g_fw(char *o, size_t c) {
  const esp_app_desc_t *d = esp_app_get_description();
  snprintf(o, c, "%s", d ? d->version : "unknown");
}

void g_ota_status(char *o, size_t c) { ota_ctrl::status(o, c); }

bool g_presence() { return presence::present(); }

// --- Entity ids (text fields on the HA device page) --------------------------

// A rejected value changes nothing: the echo that follows every command
// re-sends the stored id, so HA's field snaps back to it.
bool set_entity_text(prefs::HaEntity which, const char *domain, const char *in) {
  char buf[ha_entity_id::kMax];
  if (!ha_entity_id::normalize(in, domain, buf, sizeof buf)) {
    ESP_LOGW(TAG, "entité refusée (attendu %s.<id> en minuscules) : %s", domain,
             in != nullptr ? in : "");
    return false;
  }
  prefs::set_ha_entity(which, buf);
  ESP_LOGI(TAG, "entité %s : %s", domain, buf[0] ? buf : "(aucune)");
  return true;
}

void g_gate_id(char *o, size_t c) { prefs::ha_entity(prefs::HaEntity::kGate, o, c); }
void g_weather_id(char *o, size_t c) {
  prefs::ha_entity(prefs::HaEntity::kWeather, o, c);
}
void g_calendar_id(char *o, size_t c) {
  prefs::ha_entity(prefs::HaEntity::kCalendar, o, c);
}

void s_gate_id(const char *v) {
  if (set_entity_text(prefs::HaEntity::kGate, "cover", v)) {
    char id[ha_entity_id::kMax];
    prefs::ha_entity(prefs::HaEntity::kGate, id, sizeof id);
    gate::set_entity(id);
  }
}
void s_weather_id(const char *v) {
  (void)set_entity_text(prefs::HaEntity::kWeather, "weather", v);
}
void s_calendar_id(const char *v) {
  (void)set_entity_text(prefs::HaEntity::kCalendar, "calendar", v);
}

// --- Table ---------------------------------------------------------------------

// Entity names stay in French: they are what the household reads in HA.
// object_ids are the stable identifiers HA builds unique_ids from: never
// rename one without expecting a new entity on the HA side.
const Entity kEntities[] = {
    // --- Controls ----------------------------------------------------------
    {.kind = Kind::kLight,
     .object_id = "screen",
     .name = "Ecran",
     .icon = "mdi:tablet",
     .get_bool = g_screen_on,
     .get_float = g_screen_bri,
     .set_light = s_screen},
    {.kind = Kind::kSwitch,
     .object_id = "dnd",
     .name = "Ne pas deranger",
     .icon = "mdi:bell-off",
     .get_bool = g_dnd,
     .set_bool = s_dnd},
    {.kind = Kind::kSelect,
     .object_id = "page",
     .name = "Page affichee",
     .icon = "mdi:monitor",
     .options = kPages,
     .option_count = sizeof(kPages) / sizeof(kPages[0]),
     .get_text = g_page,
     .set_text = s_page},
    // Applied at the next pick-up (cf. prefs::echo_cancel).
    {.kind = Kind::kSelect,
     .object_id = "echo_cancel",
     .name = "Annulation d'echo",
     .icon = "mdi:account-voice",
     .options = kEchoLabels,
     .option_count = sizeof(kEchoLabels) / sizeof(kEchoLabels[0]),
     .get_text = g_echo,
     .set_text = s_echo},
    {.kind = Kind::kNumber,
     .object_id = "ring_volume",
     .name = "Volume sonnerie",
     .icon = "mdi:volume-high",
     .unit = "%",
     .min = 0,
     .max = 100,
     .step = 5,
     .get_float = g_ring_volume,
     .set_float = s_ring_volume},
    {.kind = Kind::kNumber,
     .object_id = "speaker_volume",
     .name = "Volume haut-parleur",
     .icon = "mdi:speaker",
     .unit = "%",
     .min = 0,
     .max = 100,
     .step = 5,
     .get_float = g_speaker_volume,
     .set_float = s_speaker_volume},
    {.kind = Kind::kNumber,
     .object_id = "mic_gain",
     .name = "Gain micro",
     .icon = "mdi:microphone",
     .unit = "%",
     .min = 0,
     .max = 100,
     .step = 5,
     .get_float = g_mic_gain,
     .set_float = s_mic_gain},
    {.kind = Kind::kSwitch,
     .object_id = "camera",
     .name = "Camera sans sonnerie",
     .icon = "mdi:cctv",
     .category = Category::kConfig,
     .get_bool = g_call,
     .set_bool = s_camera},
    {.kind = Kind::kSwitch,
     .object_id = "show_stats",
     .name = "Overlay statistiques",
     .category = Category::kConfig,
     .get_bool = g_stats,
     .set_bool = s_stats},
    {.kind = Kind::kSwitch,
     .object_id = "sd_log",
     .name = "Journal carte SD",
     .category = Category::kConfig,
     .get_bool = g_sdlog,
     .set_bool = s_sdlog},
    {.kind = Kind::kSwitch,
     .object_id = "file_server",
     .name = "Serveur de fichiers",
     .icon = "mdi:folder-network",
     .category = Category::kConfig,
     .get_bool = g_filesrv,
     .set_bool = s_filesrv},

    // --- Actions -----------------------------------------------------------
    // The doorbell: what an HA automation presses when the street button
    // rings (ringtone, street ding-dong, call log, missed-call snapshot).
    {.kind = Kind::kButton,
     .object_id = "ring",
     .name = "Sonner",
     .icon = "mdi:doorbell",
     .press = p_ring},
    {.kind = Kind::kButton,
     .object_id = "ack_missed",
     .name = "Acquitter les appels manques",
     .icon = "mdi:phone-check",
     .press = p_ack_missed},
    {.kind = Kind::kButton,
     .object_id = "reboot",
     .name = "Redemarrer",
     .device_class = "restart",
     .category = Category::kConfig,
     .press = p_reboot},
    {.kind = Kind::kButton,
     .object_id = "ota_sd",
     .name = "Mettre a jour depuis la carte SD",
     .device_class = "update",
     .category = Category::kConfig,
     .press = p_ota_sd},
    {.kind = Kind::kButton,
     .object_id = "presence_snapshot",
     .name = "Cliche camera de presence (SD)",
     .icon = "mdi:camera",
     .category = Category::kDiagnostic,
     .press = p_presence_snapshot},

    // --- Sensors -----------------------------------------------------------
    {.kind = Kind::kSensor,
     .object_id = "rssi",
     .name = "RSSI Wi-Fi",
     .device_class = "signal_strength",
     .unit = "dBm",
     .category = Category::kDiagnostic,
     .get_float = g_rssi},
    {.kind = Kind::kTextSensor,
     .object_id = "ip",
     .name = "Adresse IP",
     .icon = "mdi:ip-network",
     .category = Category::kDiagnostic,
     .get_text = g_ip},
    {.kind = Kind::kSensor,
     .object_id = "uptime",
     .name = "Duree de fonctionnement",
     .device_class = "duration",
     .unit = "s",
     .category = Category::kDiagnostic,
     .get_float = g_uptime},
    {.kind = Kind::kSensor,
     .object_id = "psram",
     .name = "PSRAM utilisee",
     .unit = "%",
     .category = Category::kDiagnostic,
     .get_float = g_psram},
    {.kind = Kind::kSensor,
     .object_id = "sram_free",
     .name = "SRAM interne libre",
     .device_class = "data_size",
     .unit = "B",
     .category = Category::kDiagnostic,
     .get_float = g_sram_free},
    {.kind = Kind::kSensor,
     .object_id = "cpu0",
     .name = "Charge CPU coeur 0",
     .unit = "%",
     .category = Category::kDiagnostic,
     .get_float = g_cpu0},
    {.kind = Kind::kSensor,
     .object_id = "cpu1",
     .name = "Charge CPU coeur 1",
     .unit = "%",
     .category = Category::kDiagnostic,
     .get_float = g_cpu1},
    {.kind = Kind::kSensor,
     .object_id = "sd_free",
     .name = "Espace libre carte SD",
     .unit = "MB",
     .category = Category::kDiagnostic,
     .get_float = g_sd_free,
     .sd_touching = true},
    // Not diagnostic: usage information, not maintenance.
    {.kind = Kind::kSensor,
     .object_id = "missed_calls",
     .name = "Appels manques",
     .icon = "mdi:phone-missed",
     .get_float = g_missed},
    {.kind = Kind::kTextSensor,
     .object_id = "fw_version",
     .name = "Version du firmware",
     .category = Category::kDiagnostic,
     .get_text = g_fw},
    // Entity ids used by the tablet, edited here instead of at build time.
    // max_len 127: integration-generated ids get long (ha_entity_id::kMax).
    {.kind = Kind::kText,
     .object_id = "gate_entity",
     .name = "Entite portail",
     .icon = "mdi:gate",
     .category = Category::kConfig,
     .get_text = g_gate_id,
     .set_text = s_gate_id,
     .max_len = ha_entity_id::kMax - 1,
     .pattern = ha_entity_id::kCoverPattern},
    {.kind = Kind::kText,
     .object_id = "weather_entity",
     .name = "Entite meteo",
     .icon = "mdi:weather-partly-cloudy",
     .category = Category::kConfig,
     .get_text = g_weather_id,
     .set_text = s_weather_id,
     .max_len = ha_entity_id::kMax - 1,
     .pattern = ha_entity_id::kWeatherPattern},
    {.kind = Kind::kText,
     .object_id = "calendar_entity",
     .name = "Entite agenda",
     .icon = "mdi:calendar",
     .category = Category::kConfig,
     .get_text = g_calendar_id,
     .set_text = s_calendar_id,
     .max_len = ha_entity_id::kMax - 1,
     .pattern = ha_entity_id::kCalendarPattern},
    // Pushed by ota_ctrl (changed sink in main.cpp) at each progress step and
    // after each check.
    {.kind = Kind::kUpdate,
     .object_id = "firmware",
     .name = "Firmware",
     .device_class = "firmware",
     .category = Category::kConfig,
     .get_update = g_update,
     .update_cmd = c_update,
     .refresh_when_busy = true},
    // Pushed live by ota_ctrl (report sink in main.cpp) for the progress, AND
    // read here: without a getter the entity stayed on "reboot", the last
    // message before esp_restart(), that nothing replaced at the next boot.
    {.kind = Kind::kTextSensor,
     .object_id = "ota_status",
     .name = "Statut OTA",
     .category = Category::kDiagnostic,
     .get_text = g_ota_status,
     .refresh_when_busy = true},

    // --- Binary ------------------------------------------------------------
    {.kind = Kind::kBinarySensor,
     .object_id = "in_call",
     .name = "Appel en cours",
     .device_class = "occupancy",
     .get_bool = g_call,
     .refresh_when_busy = true},
    // Tablet camera. Pushed on every change (publish_now): an automation that
    // wakes the screen cannot wait 30 s.
    {.kind = Kind::kBinarySensor,
     .object_id = "presence",
     .name = "Presence devant l'ecran",
     .device_class = "motion",
     .get_bool = g_presence,
     .refresh_when_busy = true},
};

static_assert(sizeof(kEntities) / sizeof(kEntities[0]) <=
                  esphome_api::kMaxEntities,
              "the dirty-state mask holds kMaxEntities bits");

// During a call: core 0 is saturated by the MJPEG TCP drain and SD I/O would
// add jitter to it (shared SDMMC controller with Wi-Fi).
bool busy() { return camera_session::active(); }

esphome_api::Config s_cfg = {};

} // namespace

void init(const Hooks &hooks) {
  s_hooks = hooks;
  const esp_app_desc_t *d = esp_app_get_description();
  s_cfg.name = "visiophone";
  s_cfg.friendly_name = "Visiophone";
  s_cfg.project_version = d != nullptr ? d->version : "unknown";
  s_cfg.entities = kEntities;
  s_cfg.entity_count = sizeof(kEntities) / sizeof(kEntities[0]);
  s_cfg.busy = busy;
  s_cfg.on_ha_ready = on_ha_ready;
  s_cfg.on_ha_state = gate::on_ha_state;
  if (!esphome_api::start(s_cfg)) {
    ESP_LOGE(TAG, "ESPHome API not started: Home Assistant cannot reach the board");
  }
}

void publish_now(const char *object_id) { esphome_api::publish(object_id); }

bool connected() { return esphome_api::connected(); }

} // namespace ha_api
