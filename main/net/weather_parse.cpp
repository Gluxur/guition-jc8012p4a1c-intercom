// Décodage pur des réponses Home Assistant (weather.get_forecasts et
// /api/states/<entity>) — séparé de weather.cpp (HTTP, tâche FreeRTOS, cache
// sous mutex) pour être compilable et testable par le compilateur du PC :
// cf. test/test_weather_parse.cpp. Aucune dépendance ESP-IDF au-delà du log.

#include "net/weather.h"

#include "cJSON.h"
#include "esp_log.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace weather {

static const char *TAG = "weather";

static constexpr const char *kFrDays[7] = {"Dim", "Lun", "Mar", "Mer",
                                           "Jeu", "Ven", "Sam"};

// Howard Hinnant, "chrono-compatible low-level date algorithms" :
// jours depuis l'époque civile -> jour de semaine, 0 = dimanche.
// Évite toute dépendance à l'horloge système (pas de SNTP dans ce projet) —
// le jour affiché vient du champ "datetime" de chaque entrée forecast.
static int weekday_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy =
      (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2) / 5 +
      static_cast<unsigned>(d) - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t z = era * 146097LL + static_cast<int64_t>(doe) - 719468;
  return static_cast<int>(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

bool parse_forecast_json(const char *json, Forecast &out) {
  out = Forecast{};
  cJSON *root = cJSON_Parse(json);
  if (!root) {
    ESP_LOGE(TAG, "JSON invalide");
    return false;
  }
  bool ok = false;
  do {
    // Premier enfant de service_response = l'entité, quel que soit son nom
    // exact (pas de couplage à HA_WEATHER_ENTITY ici).
    cJSON *sr = cJSON_GetObjectItem(root, "service_response");
    if (!sr || !sr->child) {
      ESP_LOGE(TAG, "service_response absent ou vide");
      break;
    }
    cJSON *fc = cJSON_GetObjectItem(sr->child, "forecast");
    if (!cJSON_IsArray(fc) || cJSON_GetArraySize(fc) == 0) {
      ESP_LOGE(TAG, "forecast absent ou vide");
      break;
    }
    int n = cJSON_GetArraySize(fc);
    if (n > 7)
      n = 7;
    for (int i = 0; i < n; ++i) {
      DayForecast &df = out.days[i];
      df.temp_max = NAN;
      df.temp_min = NAN;
      df.condition[0] = '\0';
      strlcpy(df.day, "--", sizeof df.day);
      cJSON *e = cJSON_GetArrayItem(fc, i);
      if (!e)
        continue;
      cJSON *t = cJSON_GetObjectItem(e, "temperature");
      if (cJSON_IsNumber(t))
        df.temp_max = static_cast<float>(t->valuedouble);
      t = cJSON_GetObjectItem(e, "templow");
      if (cJSON_IsNumber(t))
        df.temp_min = static_cast<float>(t->valuedouble);
      t = cJSON_GetObjectItem(e, "condition");
      if (cJSON_IsString(t))
        strlcpy(df.condition, t->valuestring, sizeof df.condition);
      t = cJSON_GetObjectItem(e, "datetime");
      int y = 0, mo = 0, d = 0;
      if (cJSON_IsString(t) &&
          sscanf(t->valuestring, "%d-%d-%d", &y, &mo, &d) == 3)
        strlcpy(df.day, kFrDays[weekday_from_civil(y, mo, d)], sizeof df.day);
      ESP_LOGD(TAG, "J+%d: %s max=%.1f min=%.1f cond=%s", i, df.day,
               df.temp_max, df.temp_min, df.condition);
    }
    out.count = n;
    ok = true;
  } while (0);
  cJSON_Delete(root);
  return ok;
}

bool parse_current_json(const char *json, CurrentConditions &out) {
  out = CurrentConditions{};
  out.temperature = NAN;
  out.humidity = NAN;
  out.wind_speed = NAN;
  out.pressure = NAN;
  out.condition[0] = '\0';
  cJSON *root = cJSON_Parse(json);
  if (!root) {
    ESP_LOGE(TAG, "état JSON invalide");
    return false;
  }
  cJSON *state = cJSON_GetObjectItem(root, "state");
  if (cJSON_IsString(state))
    strlcpy(out.condition, state->valuestring, sizeof out.condition);
  cJSON *attrs = cJSON_GetObjectItem(root, "attributes");
  if (attrs) {
    cJSON *t = cJSON_GetObjectItem(attrs, "temperature");
    if (cJSON_IsNumber(t))
      out.temperature = static_cast<float>(t->valuedouble);
    t = cJSON_GetObjectItem(attrs, "humidity");
    if (cJSON_IsNumber(t))
      out.humidity = static_cast<float>(t->valuedouble);
    t = cJSON_GetObjectItem(attrs, "pressure");
    if (cJSON_IsNumber(t))
      out.pressure = static_cast<float>(t->valuedouble);
    t = cJSON_GetObjectItem(attrs, "wind_speed");
    if (cJSON_IsNumber(t))
      out.wind_speed = static_cast<float>(t->valuedouble);
  }
  out.valid = true;
  cJSON_Delete(root);
  return true;
}

} // namespace weather
