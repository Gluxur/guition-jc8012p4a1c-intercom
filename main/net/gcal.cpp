#include "net/gcal.h"

#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sys/mutex.h"
#include "net/esphome_api.h"
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

static void format_utc(int64_t epoch_s, char *dst, size_t cap) {
  time_t t = static_cast<time_t>(epoch_s);
  struct tm g;
  gmtime_r(&t, &g);
  strftime(dst, cap, "%Y-%m-%dT%H:%M:%S+00:00", &g);
}

// calendar.get_events answers {"<entity>": {"events": [{"start": "...",
// "end": "...", "summary": ...}]}} with bare strings: "2026-07-30T10:00:00+02:00"
// for a timed event, "2026-07-30" for an all-day one. The template turns that
// into the layout gcal_parse already decodes (REST API: {"dateTime"|"date"}),
// and drops description/location, which the screen never shows.
static void build_template(char *out, size_t cap) {
  snprintf(out, cap,
           "{%% set ns = namespace(l=[]) %%}"
           "{%% for ev in response['%s'].events %%}"
           "{%% set k = 'dateTime' if 'T' in ev['start'] else 'date' %%}"
           "{%% set ns.l = ns.l + [{'summary': ev.get('summary', ''),"
           " 'start': {k: ev['start']}, 'end': {k: ev['end']}}] %%}"
           "{%% endfor %%}{{ ns.l }}",
           cfg::kHaCalendarEntity);
}

static bool do_fetch(int64_t range_start_s, int64_t range_end_s) {
  char start_str[32], end_str[32];
  format_utc(range_start_s, start_str, sizeof start_str);
  format_utc(range_end_s, end_str, sizeof end_str);
  char *resp =
      static_cast<char *>(heap_caps_malloc(kRespBufSize, MALLOC_CAP_SPIRAM));
  if (resp == nullptr) {
    ESP_LOGE(TAG, "alloc buffer réponse échouée");
    return false;
  }
  char tmpl[512];
  build_template(tmpl, sizeof tmpl);
  const esphome_api::Kv data[] = {
      {"entity_id", cfg::kHaCalendarEntity},
      {"start_date_time", start_str},
      {"end_date_time", end_str},
  };
  size_t len = 0;
  if (!esphome_api::call_action_sync("calendar.get_events", data, 3, tmpl,
                                     resp, kRespBufSize, &len)) {
    heap_caps_free(resp);
    return false;
  }
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
