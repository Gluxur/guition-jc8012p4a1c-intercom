#pragma once

#include <cstdint>

namespace weather {

struct DayForecast {
  char day[4];         // "Lun".."Dim" — dérivé du champ datetime (pas de SNTP)
  float temp_max;      // NAN si champ "temperature" absent
  float temp_min;      // NAN si champ "templow" absent
  char condition[24];  // "sunny", "partlycloudy"… ; "" si absent
};

struct CurrentConditions {
  float temperature;   // NAN si "attributes.temperature" absent
  float humidity;      // NAN si "attributes.humidity" absent
  float wind_speed;    // NAN si "attributes.wind_speed" absent
  float pressure;      // NAN si "attributes.pressure" absent
  char condition[24];  // état de l'entité weather.* ("sunny"...) ; "" si absent
  bool valid;           // true si le GET état a réussi (même si champs partiels)
};

struct Forecast {
  DayForecast days[7];
  CurrentConditions current;
  int count = 0;             // jours réellement reçus (<= 7)
  bool valid = false;
  int64_t fetched_at_us = 0; // esp_timer_get_time() à la fin du fetch
};

/* Parses the weather.get_forecasts action response reshaped by weather.cpp's
 * template: response -> forecast[]. Fills at most 7 days. false if the JSON
 * is invalid or forecast missing/empty. Defined in weather_parse.cpp (pure
 * logic, host-tested: test/test_weather_parse.cpp). */
bool parse_forecast_json(const char *json, Forecast &out);

/* Same document, response -> current: "state" (condition) +
 * "attributes.{temperature,humidity,pressure,wind_speed}". true if the JSON
 * is valid and `current` present (missing fields -> NAN, not a failure).
 * Defined in weather_parse.cpp. */
bool parse_current_json(const char *json, CurrentConditions &out);

/* Copie du cache, prise sous verrou. PAS une référence : la tâche weather_fetch
 * réécrit la structure entière (~310 octets) pendant que la tâche LVGL la lit
 * depuis ses timers et son LV_EVENT_SCREEN_LOADED — une référence exposait une
 * lecture déchirée (jours mélangés, `condition` non terminé). ~310 octets de
 * pile chez l'appelant, négligeable sur les 48 Ko de lvgl_task. */
[[nodiscard]] Forecast cached();
[[nodiscard]] bool cache_stale(int64_t max_age_us);

/* Calls weather.get_forecasts through the ESPHome API (no token: HA runs the
 * action for the device) and parses it, in a short self-deleting task.
 * done_cb(ok) est appelé depuis cette tâche — prendre le mutex LVGL avant
 * tout lv_*. Si un fetch est déjà en cours, l'appel est ignoré. `ok` reflète
 * uniquement les prévisions ; les conditions actuelles, dans la même réponse,
 * sont best-effort et n'affectent pas `ok`. */
void fetch_async(void (*done_cb)(bool ok));

} // namespace weather
