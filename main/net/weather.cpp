#include "weather.h"

#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sys/mutex.h"
#include "sys/settings.h"
#include <atomic>
#include <cstdio>
#include <cstring>

namespace weather {

static const char *TAG = "weather";

// Hôte et jeton viennent de settings (NVS ou défauts compilés), l'entité de
// app_config.h. Ils ne sont donc plus connus à la compilation : les URLs et
// l'en-tête d'autorisation se construisent par snprintf, là où l'ancien code
// s'appuyait sur la concaténation de littéraux du préprocesseur.

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

struct RespBuf {
  char *data;
  int len;
  int cap;
  bool overflow; // réponse plus grosse que le buffer : JSON forcément invalide
};

// Même pattern d'accumulation que http_event_handler (signaling main.cpp),
// mais buffer passé par user_data : pas de statique partagée.
static esp_err_t http_event(esp_http_client_event_t *evt) {
  auto *rb = static_cast<RespBuf *>(evt->user_data);
  switch (evt->event_id) {
  case HTTP_EVENT_ON_DATA:
    if (rb->len + evt->data_len < rb->cap - 1) {
      memcpy(rb->data + rb->len, evt->data, evt->data_len);
      rb->len += evt->data_len;
    } else {
      rb->overflow = true;
    }
    break;
  case HTTP_EVENT_ON_FINISH:
    rb->data[rb->len] = '\0';
    break;
  default:
    break;
  }
  return ESP_OK;
}

// Exécute une requête à l'API Home Assistant et renvoie le corps de la réponse
// dans un buffer PSRAM que l'APPELANT libère (heap_caps_free), ou nullptr en cas
// d'échec réseau/HTTP. `json_body` non nul => POST avec ce corps, sinon GET.
// Factorisé : les deux appels (prévisions et état courant) ne différaient que
// par l'URL, la méthode et le corps — dupliquer l'en-tête d'auth, le buffer et
// le traitement d'erreur laissait deux endroits à corriger pour un seul bug.
static char *ha_request(const char *url, const char *json_body,
                        const char *what) {
  RespBuf rb = {};
  rb.cap = kRespBufSize;
  rb.data =
      static_cast<char *>(heap_caps_malloc(kRespBufSize, MALLOC_CAP_SPIRAM));
  if (!rb.data) {
    ESP_LOGE(TAG, "%s: alloc buffer réponse échouée", what);
    return nullptr;
  }

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.event_handler = http_event;
  cfg.user_data = &rb;
  cfg.timeout_ms = 10000;
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    ESP_LOGE(TAG, "%s: esp_http_client_init échoué", what);
    heap_caps_free(rb.data);
    return nullptr;
  }
  // "Bearer <jeton>" : le jeton HA fait ~250 caractères, la pile de la tâche
  // weather_fetch (8 Ko) l'absorbe sans problème mais la borne reste explicite.
  char auth[settings::kTokenMax + 8];
  snprintf(auth, sizeof auth, "Bearer %s", settings::get().ha_token);
  esp_http_client_set_header(client, "Authorization", auth);
  if (json_body != nullptr) {
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_body, strlen(json_body));
  } else {
    esp_http_client_set_method(client, HTTP_METHOD_GET);
  }

  bool ok = false;
  esp_err_t err = esp_http_client_perform(client);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "%s: HTTP %s", what, esp_err_to_name(err));
  } else {
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
      ESP_LOGE(TAG, "%s: HTTP status %d (token/entité ?)", what, status);
    } else if (rb.overflow) {
      // Le corps tronqué ne parserait pas de toute façon : nommer la cause ici
      // évite un « JSON invalide » qui pointerait vers Home Assistant.
      ESP_LOGE(TAG, "%s: réponse > %d o, tronquée (%d o reçus)", what, rb.cap,
               rb.len);
    } else {
      ok = true;
    }
  }
  esp_http_client_cleanup(client);
  if (!ok) {
    heap_caps_free(rb.data);
    return nullptr;
  }
  return rb.data;
}

static bool do_fetch() {
  char body[256];
  snprintf(body, sizeof body, "{\"entity_id\":\"%s\",\"type\":\"daily\"}",
           cfg::kHaWeatherEntity);
  char url[192];
  snprintf(url, sizeof url,
           "%s/api/services/weather/get_forecasts?return_response",
           settings::ha_base_url());
  char *resp = ha_request(url, body, "prévisions");
  if (resp == nullptr) {
    return false;
  }
  bool ok = false;
  Forecast fresh;
  if (parse_forecast_json(resp, fresh)) {
    fresh.valid = true;
    fresh.fetched_at_us = esp_timer_get_time();
    // Les conditions actuelles sont récupérées séparément (do_fetch_current) et
    // ne doivent pas être perdues par cette publication : on les reprend du
    // cache, sous le même verrou.
    sys::MutexLock lock(s_cache_mux);
    fresh.current = s_cache.current;
    s_cache = fresh;
    ok = true;
  }
  heap_caps_free(resp);
  return ok;
}

static bool do_fetch_current(CurrentConditions &out) {
  char url[192];
  snprintf(url, sizeof url, "%s/api/states/%s", settings::ha_base_url(),
           cfg::kHaWeatherEntity);
  char *resp = ha_request(url, nullptr, "état");
  if (resp == nullptr) {
    return false;
  }
  bool ok = parse_current_json(resp, out);
  heap_caps_free(resp);
  return ok;
}

static void fetch_task(void *) {
  int64_t t_start = esp_timer_get_time();
  bool ok = do_fetch();
  int64_t t_forecast = esp_timer_get_time();
  ESP_LOGI(TAG, "get_forecasts: %lld ms (%s)", (t_forecast - t_start) / 1000,
           ok ? "ok" : "échec");

  CurrentConditions current;
  if (do_fetch_current(current)) {
    sys::MutexLock lock(s_cache_mux);
    s_cache.current = current;
  } else {
    ESP_LOGE(TAG, "fetch conditions actuelles échoué (banner en --)");
  }
  int64_t t_current = esp_timer_get_time();
  ESP_LOGI(TAG, "état actuel: %lld ms", (t_current - t_forecast) / 1000);

  int days = 0;
  {
    sys::MutexLock lock(s_cache_mux);
    days = s_cache.count;
  }
  ESP_LOGI(TAG, "fetch %s (%d jours) — total %lld ms", ok ? "ok" : "échec", days,
           (t_current - t_start) / 1000);
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
