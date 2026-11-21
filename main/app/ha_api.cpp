#include "ha_api.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "app/camera_session.h"
#include "app/presence.h"
#include "app_config.h"
#include "bsp.h"
#include "net/esphome_api.h"

namespace ha_api {
namespace {

constexpr const char *TAG = "ha_api";

Hooks s_hooks = {};

bool g_presence() { return presence::present(); }

float g_speaker_volume() {
  return (float)bsp::Bsp::instance().speaker_volume();
}

void s_speaker_volume(float v) {
  if (v < 0.0f || v > 100.0f) {
    return; // the echo republishes the real value
  }
  bsp::Bsp::instance().set_speaker_volume((uint8_t)lroundf(v));
}

void p_open_camera() {
  if (s_hooks.ring != nullptr) {
    s_hooks.ring();
  }
}

const esphome_api::Entity kEntities[] = {
    {.kind = esphome_api::Kind::kButton,
     .object_id = "open_camera",
     .name = "Ouvrir l'ecran camera",
     .icon = "mdi:video",
     .press = p_open_camera},
    {.kind = esphome_api::Kind::kNumber,
     .object_id = "speaker_volume",
     .name = "Volume haut-parleur",
     .icon = "mdi:speaker",
     .unit = "%",
     .min = 0,
     .max = 100,
     .step = 5,
     .get_float = g_speaker_volume,
     .set_float = s_speaker_volume},
    {.kind = esphome_api::Kind::kBinarySensor,
     .object_id = "presence",
     .name = "Presence devant l'ecran",
     .device_class = "motion",
     .get_bool = g_presence},
};

// --- Prototype probe: weather.get_forecasts through HA, no token -----------

void on_forecast(bool ok, const char *error, const char *json, size_t len) {
  if (!ok) {
    ESP_LOGW(TAG, "get_forecasts: failed (%s)", error);
    return;
  }
  ESP_LOGI(TAG, "get_forecasts: %u bytes: %.*s%s", (unsigned)len,
           len > 160 ? 160 : (int)len, json, len > 160 ? "..." : "");
  ESP_LOGI(TAG, "internal SRAM free %u, min %u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

void on_ha_ready() {
  const esphome_api::Kv data[] = {
      {"entity_id", cfg::kHaWeatherEntity},
      {"type", "daily"},
  };
  if (!esphome_api::call_action("weather.get_forecasts", data, 2, nullptr,
                                on_forecast)) {
    ESP_LOGW(TAG, "get_forecasts not queued");
  }
}

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
  if (!esphome_api::start(s_cfg)) {
    ESP_LOGE(TAG, "ESPHome API not started");
  }
}

void publish_now(const char *object_id) { esphome_api::publish(object_id); }

} // namespace ha_api
