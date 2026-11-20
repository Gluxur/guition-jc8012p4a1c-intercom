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

/* Parse la réponse du service weather.get_forecasts (?return_response) :
 * service_response -> <première entité> -> forecast[]. Remplit au plus
 * 7 jours. false si JSON invalide ou forecast absent/vide. Défini dans
 * weather_parse.cpp (logique pure, testée sur host :
 * test/test_weather_parse.cpp). */
bool parse_forecast_json(const char *json, Forecast &out);

/* Parse la réponse GET /api/states/<entity> : "state" (condition) +
 * "attributes.{temperature,humidity,pressure,wind_speed}". Retourne true
 * si le JSON est valide (champs absents -> NAN, pas un échec). Défini dans
 * weather_parse.cpp. */
bool parse_current_json(const char *json, CurrentConditions &out);

/* Copie du cache, prise sous verrou. PAS une référence : la tâche weather_fetch
 * réécrit la structure entière (~310 octets) pendant que la tâche LVGL la lit
 * depuis ses timers et son LV_EVENT_SCREEN_LOADED — une référence exposait une
 * lecture déchirée (jours mélangés, `condition` non terminé). ~310 octets de
 * pile chez l'appelant, négligeable sur les 48 Ko de lvgl_task. */
[[nodiscard]] Forecast cached();
[[nodiscard]] bool cache_stale(int64_t max_age_us);

/* Lance fetch HTTP + parse dans une tâche FreeRTOS courte auto-détruite.
 * done_cb(ok) est appelé depuis cette tâche — prendre le mutex LVGL avant
 * tout lv_*. Si un fetch est déjà en cours, l'appel est ignoré. `ok` reflète
 * uniquement le fetch forecast (get_forecasts) ; le fetch des conditions
 * actuelles (GET état) est best-effort et n'affecte pas `ok`. */
void fetch_async(void (*done_cb)(bool ok));

} // namespace weather
