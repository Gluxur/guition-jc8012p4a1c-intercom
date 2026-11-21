#include "weather.h"

#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net/esphome_api.h"
#include "sys/mutex.h"
#include <atomic>
#include <cstdio>
#include <cstring>

namespace weather {

static const char *TAG = "weather";

// Réponse réelle ~3-6 Ko ; marge x5. PSRAM : règle projet buffers > 4 Ko.
static constexpr int kRespBufSize = 32 * 1024;

static Forecast s_cache;
// Le cache est écrit par la tâche weather_fetch et lu par la tâche LVGL
// (ui_weather_v3 : SCREEN_LOADED, timers horloge/refresh). Une affectation de
// structure n'est pas atomique : sans ce mutex l'écran pouvait lire un mélange
// de deux prévisions, voire un `condition[24]` sans terminateur.
static SemaphoreHandle_t s_cache_mux = nullptr;

// Création paresseuse. Tous les premiers appelants possibles (cached(),
// cache_stale(), fetch_async()) viennent de la tâche LVGL — la tâche
// weather_fetch, seule autre à toucher le cache, n'existe qu'après un
// fetch_async : pas de course sur la création elle-même.
static SemaphoreHandle_t cache_mux() {
  if (s_cache_mux == nullptr) {
    s_cache_mux = xSemaphoreCreateMutex();
  }
  return s_cache_mux;
}

// parse_forecast_json / parse_current_json vivent dans weather_parse.cpp :
// logique pure, testée sur host.

static std::atomic<bool> s_fetch_in_flight{false};
static void (*s_done_cb)(bool) = nullptr;

// One action call for both the forecast and the current conditions: HA
// renders this template with `response` = the action's answer, so the board
// receives only what it displays (7 days, 4 attributes) and no second request
// is needed for the entity state. The result is a dict, which HA sends back as
// JSON under "response".
static void build_template(char *out, size_t cap) {
  snprintf(out, cap,
           "{%% set e = '%s' %%}"
           "{{ {'forecast': response[e].forecast[:7],"
           " 'current': {'state': states(e), 'attributes': {"
           "'temperature': state_attr(e, 'temperature'),"
           "'humidity': state_attr(e, 'humidity'),"
           "'pressure': state_attr(e, 'pressure'),"
           "'wind_speed': state_attr(e, 'wind_speed')}}} }}",
           cfg::kHaWeatherEntity);
}

static bool do_fetch() {
  char *resp =
      static_cast<char *>(heap_caps_malloc(kRespBufSize, MALLOC_CAP_SPIRAM));
  if (resp == nullptr) {
    ESP_LOGE(TAG, "alloc buffer réponse échouée");
    return false;
  }
  char tmpl[512];
  build_template(tmpl, sizeof tmpl);
  const esphome_api::Kv data[] = {
      {"entity_id", cfg::kHaWeatherEntity},
      {"type", "daily"},
  };
  size_t len = 0;
  if (!esphome_api::call_action_sync("weather.get_forecasts", data, 2, tmpl,
                                     resp, kRespBufSize, &len)) {
    // No HA connected yet, actions not allowed for the device, wrong entity:
    // the API module logged which.
    heap_caps_free(resp);
    return false;
  }
  bool ok = false;
  Forecast fresh;
  if (parse_forecast_json(resp, fresh)) {
    fresh.valid = true;
    fresh.fetched_at_us = esp_timer_get_time();
    if (!parse_current_json(resp, fresh.current)) {
      ESP_LOGW(TAG, "conditions actuelles absentes (bandeau en --)");
    }
    sys::MutexLock lock(s_cache_mux);
    s_cache = fresh;
    ok = true;
  }
  heap_caps_free(resp);
  return ok;
}

static void fetch_task(void *) {
  const int64_t t_start = esp_timer_get_time();
  const bool ok = do_fetch();
  int days = 0;
  {
    sys::MutexLock lock(s_cache_mux);
    days = s_cache.count;
  }
  ESP_LOGI(TAG, "fetch %s (%d jours) — %lld ms", ok ? "ok" : "échec", days,
           (esp_timer_get_time() - t_start) / 1000);
  void (*cb)(bool) = s_done_cb;
  s_fetch_in_flight.store(false);
  if (cb)
    cb(ok);
  vTaskDelete(nullptr);
}

void fetch_async(void (*done_cb)(bool ok)) {
  if (cache_mux() == nullptr) {
    ESP_LOGE(TAG, "création du mutex de cache échouée");
    if (done_cb)
      done_cb(false);
    return;
  }
  bool expected = false;
  if (!s_fetch_in_flight.compare_exchange_strong(expected, true)) {
    ESP_LOGD(TAG, "fetch déjà en cours, ignoré");
    return;
  }
  s_done_cb = done_cb;
  // Priorité 3 < tâches vidéo/audio ; non épinglée (aucune contrainte RT).
  if (xTaskCreate(fetch_task, "weather_fetch", 8192, nullptr, 3, nullptr) !=
      pdPASS) {
    s_fetch_in_flight.store(false);
    ESP_LOGE(TAG, "création tâche fetch échouée");
    if (done_cb)
      done_cb(false);
  }
}

Forecast cached() {
  SemaphoreHandle_t mux = cache_mux();
  if (mux == nullptr) {
    return s_cache; // pas de mutex allouable : dégradé, mais jamais un crash
  }
  sys::MutexLock lock(mux);
  return s_cache;
}

bool cache_stale(int64_t max_age_us) {
  SemaphoreHandle_t mux = cache_mux();
  bool valid = false;
  int64_t fetched_at = 0;
  if (mux != nullptr) {
    sys::MutexLock lock(mux);
    valid = s_cache.valid;
    fetched_at = s_cache.fetched_at_us;
  } else {
    valid = s_cache.valid;
    fetched_at = s_cache.fetched_at_us;
  }
  return !valid || (esp_timer_get_time() - fetched_at) > max_age_us;
}

} // namespace weather
