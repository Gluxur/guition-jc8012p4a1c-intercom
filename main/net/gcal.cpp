#include "net/gcal.h"

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
#include <ctime>

namespace gcal {

static const char *TAG = "gcal";

// Réponse réelle attendue < 8 Ko pour une fenêtre semaine/mois d'un agenda
// perso ; marge x4. PSRAM : règle projet buffers > 4 Ko.
static constexpr int kRespBufSize = 32 * 1024;

static EventList s_cache;
static SemaphoreHandle_t s_cache_mux = nullptr;

static SemaphoreHandle_t cache_mux() {
  if (s_cache_mux == nullptr) {
    s_cache_mux = xSemaphoreCreateMutex();
  }
  return s_cache_mux;
}

// parse_events_json vit dans gcal_parse.cpp : logique pure, testée sur host.

static std::atomic<bool> s_fetch_in_flight{false};
static void (*s_done_cb)(bool) = nullptr;
static int64_t s_pending_start_s = 0;
static int64_t s_pending_end_s = 0;

struct RespBuf {
  char *data;
  int len;
  int cap;
  bool overflow; // réponse plus grosse que le buffer : JSON forcément invalide
};

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

// GET simple (l'API calendrier HA ne prend pas de corps). Dupliqué de
// weather::ha_request faute d'utilitaire HTTP HA partagé dans le projet.
static char *ha_get(const char *url, const char *what) {
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
  char auth[settings::kTokenMax + 8];
  snprintf(auth, sizeof auth, "Bearer %s", settings::get().ha_token);
  esp_http_client_set_header(client, "Authorization", auth);
  esp_http_client_set_method(client, HTTP_METHOD_GET);

  bool ok = false;
  esp_err_t err = esp_http_client_perform(client);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "%s: HTTP %s", what, esp_err_to_name(err));
  } else {
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
      ESP_LOGE(TAG, "%s: HTTP status %d (token/entité ?)", what, status);
    } else if (rb.overflow) {
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

static void format_utc(int64_t epoch_s, char *dst, size_t cap) {
  time_t t = static_cast<time_t>(epoch_s);
  struct tm g;
  gmtime_r(&t, &g);
  strftime(dst, cap, "%Y-%m-%dT%H:%M:%SZ", &g);
}

static bool do_fetch(int64_t range_start_s, int64_t range_end_s) {
  char start_str[24], end_str[24];
  format_utc(range_start_s, start_str, sizeof start_str);
  format_utc(range_end_s, end_str, sizeof end_str);
  char url[224];
  snprintf(url, sizeof url, "%s/api/calendars/%s?start=%s&end=%s",
           settings::ha_base_url(), cfg::kHaCalendarEntity, start_str,
           end_str);
  char *resp = ha_get(url, "agenda");
  if (resp == nullptr)
    return false;
  bool ok = false;
  EventList fresh;
  if (parse_events_json(resp, fresh)) {
    fresh.valid = true;
    fresh.range_start_s = range_start_s;
    fresh.range_end_s = range_end_s;
    fresh.fetched_at_us = esp_timer_get_time();
    sys::MutexLock lock(cache_mux());
    s_cache = fresh;
    ok = true;
  }
  heap_caps_free(resp);
  return ok;
}

// Requête arrivée pendant un fetch en vol : elle était jetée, et son appelant
// jamais rappelé (Météo puis Agenda en moins d'une seconde, ou deux appuis
// rapides sur la flèche de semaine : grille fausse jusqu'au poll de 15 min).
// La dernière arrivée gagne — c'est elle qui décrit la vue affichée — et
// fetch_task la rejoue en sortant. Section critique et non mutex : quelques
// affectations, prises aussi depuis la tâche LVGL.
static portMUX_TYPE s_queue_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_queued = false;
static int64_t s_queued_start_s = 0;
static int64_t s_queued_end_s = 0;
static void (*s_queued_cb)(bool) = nullptr;

static void fetch_task(void *) {
  int64_t start_s = s_pending_start_s;
  int64_t end_s = s_pending_end_s;
  void (*cb)(bool) = s_done_cb;
  for (;;) {
    int64_t t0 = esp_timer_get_time();
    bool ok = do_fetch(start_s, end_s);
    int count = ok ? cached().count : 0;
    ESP_LOGI(TAG, "fetch %s (%d événements) — %lld ms", ok ? "ok" : "échec",
             count, (esp_timer_get_time() - t0) / 1000);
    bool again = false;
    void (*next_cb)(bool) = nullptr;
    taskENTER_CRITICAL(&s_queue_mux);
    if (s_queued) {
      s_queued = false;
      again = true;
      start_s = s_queued_start_s;
      end_s = s_queued_end_s;
      next_cb = s_queued_cb;
    } else {
      s_fetch_in_flight.store(false);
    }
    taskEXIT_CRITICAL(&s_queue_mux);
    if (cb)
      cb(ok);
    if (!again)
      break;
    cb = next_cb;
  }
  vTaskDelete(nullptr);
}

void fetch_async(int64_t range_start_s, int64_t range_end_s,
                 void (*done_cb)(bool ok)) {
  if (cache_mux() == nullptr) {
    ESP_LOGE(TAG, "création du mutex de cache échouée");
    if (done_cb)
      done_cb(false);
    return;
  }
  taskENTER_CRITICAL(&s_queue_mux);
  if (s_fetch_in_flight.load()) {
    s_queued = true;
    s_queued_start_s = range_start_s;
    s_queued_end_s = range_end_s;
    s_queued_cb = done_cb;
    taskEXIT_CRITICAL(&s_queue_mux);
    ESP_LOGD(TAG, "fetch en cours : requête rejouée à sa fin");
    return;
  }
  s_fetch_in_flight.store(true);
  taskEXIT_CRITICAL(&s_queue_mux);
  s_pending_start_s = range_start_s;
  s_pending_end_s = range_end_s;
  s_done_cb = done_cb;
  // Priorité 3 < tâches vidéo/audio ; non épinglée (aucune contrainte RT).
  // 16 Ko et non 8 Ko (comme weather_fetch) : done_cb tourne SUR cette pile
  // (appelé directement avant vTaskDelete, cf. fetch_task ci-dessus) et copie
  // localement un gcal::EventList entier (~4,6 Ko, kMax=64) avant de faire du
  // travail LVGL par-dessus (ui_agenda::on_fetch_done /
  // ui_weather_v3::on_gcal_fetch_done) — weather::Forecast, lui, ne pèse que
  // ~310 o, d'où le 8 Ko historique de weather_fetch. Constaté sur cible :
  // Stack protection fault dans gcal_fetch juste après le fetch.
  if (xTaskCreate(fetch_task, "gcal_fetch", 16384, nullptr, 3, nullptr) !=
      pdPASS) {
    s_fetch_in_flight.store(false);
    ESP_LOGE(TAG, "création tâche fetch échouée");
    if (done_cb)
      done_cb(false);
  }
}

EventList cached() {
  SemaphoreHandle_t mux = cache_mux();
  if (mux == nullptr)
    return s_cache; // pas de mutex allouable : dégradé, jamais un crash
  sys::MutexLock lock(mux);
  return s_cache;
}

bool cache_covers(int64_t start_s, int64_t end_s, int64_t max_age_us) {
  // Sous verrou : `range_covers` lit quatre champs, et la tâche gcal_fetch
  // remplace la structure entière d'un bloc.
  SemaphoreHandle_t mux = cache_mux();
  const int64_t now_us = esp_timer_get_time();
  if (mux == nullptr)
    return range_covers(s_cache, start_s, end_s, now_us, max_age_us);
  sys::MutexLock lock(mux);
  return range_covers(s_cache, start_s, end_s, now_us, max_age_us);
}

} // namespace gcal
