#include "net/weather.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace {

// Enveloppe la réponse du service weather.get_forecasts : le parser doit
// prendre le PREMIER enfant de service_response quel que soit son nom, donc les
// tests varient délibérément le nom d'entité.
std::string wrap(const std::string &forecast_array,
                 const char *entity = "weather.maison") {
  return std::string("{\"service_response\":{\"") + entity +
         "\":{\"forecast\":" + forecast_array + "}}}";
}

weather::Forecast parse(const std::string &json, bool expect_ok = true) {
  weather::Forecast f;
  EXPECT_EQ(expect_ok, weather::parse_forecast_json(json.c_str(), f));
  return f;
}

// --- Entrées malformées -----------------------------------------------------

TEST(WeatherParse, RejectsMalformedJson) {
  weather::Forecast f;
  EXPECT_FALSE(weather::parse_forecast_json("{ pas du json", f));
}

TEST(WeatherParse, RejectsAMissingServiceResponse) {
  weather::Forecast f;
  EXPECT_FALSE(weather::parse_forecast_json("{\"autre\":1}", f));
}

TEST(WeatherParse, RejectsAnEmptyServiceResponse) {
  weather::Forecast f;
  EXPECT_FALSE(weather::parse_forecast_json("{\"service_response\":{}}", f));
}

TEST(WeatherParse, RejectsAnEmptyForecastArray) {
  // Un tableau vide ne remplirait aucune carte : mieux vaut garder l'ancien
  // cache affiché que 7 cartes "--".
  weather::Forecast f;
  EXPECT_FALSE(weather::parse_forecast_json(wrap("[]").c_str(), f));
}

TEST(WeatherParse, RejectsAForecastThatIsNotAnArray) {
  weather::Forecast f;
  EXPECT_FALSE(weather::parse_forecast_json(wrap("{}").c_str(), f));
}

TEST(WeatherParse, LeavesCountAtZeroWhenParsingFails) {
  weather::Forecast f;
  ASSERT_FALSE(weather::parse_forecast_json("{ pas du json", f));
  EXPECT_EQ(0, f.count);
  EXPECT_FALSE(f.valid);
}

// --- Nom d'entité -----------------------------------------------------------

TEST(WeatherParse, IgnoresTheEntityName) {
  // Le parser ne doit pas être couplé à cfg::kHaWeatherEntity : renommer
  // l'entité côté HA ne doit pas éteindre l'écran météo.
  const weather::Forecast f = parse(
      wrap("[{\"datetime\":\"2026-07-11T12:00:00+00:00\","
           "\"temperature\":28.4}]",
           "weather.un_nom_totalement_different"));
  EXPECT_EQ(1, f.count);
}

// --- Jour de la semaine -----------------------------------------------------

TEST(WeatherParse, DerivesTheWeekdayFromTheDatetimeField) {
  // Pas de SNTP fiable au boot : le jour vient du champ datetime, pas de
  // l'horloge. 2026-07-11 est un samedi (2026-01-01 = jeudi + 191 jours).
  const weather::Forecast f =
      parse(wrap("[{\"datetime\":\"2026-07-11T12:00:00+00:00\"},"
                 "{\"datetime\":\"2026-07-12T12:00:00+00:00\"},"
                 "{\"datetime\":\"2026-07-13T12:00:00+00:00\"}]"));
  ASSERT_EQ(3, f.count);
  EXPECT_STREQ("Sam", f.days[0].day);
  EXPECT_STREQ("Dim", f.days[1].day);
  EXPECT_STREQ("Lun", f.days[2].day);
}

TEST(WeatherParse, HandlesLeapDayBoundaries) {
  // 2024 est bissextile : le 29 février existe (jeudi) et le 1er mars le suit
  // (vendredi). Un algorithme civil cassé décale tout le reste de l'année.
  const weather::Forecast f =
      parse(wrap("[{\"datetime\":\"2024-02-29T12:00:00+00:00\"},"
                 "{\"datetime\":\"2024-03-01T12:00:00+00:00\"}]"));
  ASSERT_EQ(2, f.count);
  EXPECT_STREQ("Jeu", f.days[0].day);
  EXPECT_STREQ("Ven", f.days[1].day);
}

TEST(WeatherParse, HandlesTheYear2100NonLeapRule) {
  // 2100 n'est PAS bissextile (règle des multiples de 100) : c'est la branche
  // que les implémentations naïves ratent. 2100-03-01 est un lundi.
  const weather::Forecast f =
      parse(wrap("[{\"datetime\":\"2100-03-01T12:00:00+00:00\"}]"));
  ASSERT_EQ(1, f.count);
  EXPECT_STREQ("Lun", f.days[0].day);
}

TEST(WeatherParse, FallsBackToADashWhenDatetimeIsMissingOrUnparseable) {
  const weather::Forecast f =
      parse(wrap("[{\"temperature\":20.0},"
                 "{\"datetime\":\"pas une date\",\"temperature\":21.0}]"));
  ASSERT_EQ(2, f.count);
  EXPECT_STREQ("--", f.days[0].day);
  EXPECT_STREQ("--", f.days[1].day);
}

// --- Valeurs ----------------------------------------------------------------

TEST(WeatherParse, ReadsTemperaturesAndCondition) {
  const weather::Forecast f =
      parse(wrap("[{\"datetime\":\"2026-07-11T12:00:00+00:00\","
                 "\"condition\":\"sunny\",\"temperature\":28.4,"
                 "\"templow\":16.2}]"));
  ASSERT_EQ(1, f.count);
  EXPECT_NEAR(28.4f, f.days[0].temp_max, 0.01f);
  EXPECT_NEAR(16.2f, f.days[0].temp_min, 0.01f);
  EXPECT_STREQ("sunny", f.days[0].condition);
}

TEST(WeatherParse, ReadsNegativeTemperatures) {
  const weather::Forecast f =
      parse(wrap("[{\"datetime\":\"2026-01-15T12:00:00+00:00\","
                 "\"temperature\":-3.5,\"templow\":-11.0}]"));
  ASSERT_EQ(1, f.count);
  EXPECT_NEAR(-3.5f, f.days[0].temp_max, 0.01f);
  EXPECT_NEAR(-11.0f, f.days[0].temp_min, 0.01f);
}

TEST(WeatherParse, UsesNanForMissingTemperatures) {
  // NAN et pas 0 : l'UI affiche "--" sur NAN, alors qu'un 0 s'afficherait
  // comme une vraie prévision à 0 °C.
  const weather::Forecast f =
      parse(wrap("[{\"datetime\":\"2026-07-13T12:00:00+00:00\","
                 "\"temperature\":19.0}]"));
  ASSERT_EQ(1, f.count);
  EXPECT_FALSE(std::isnan(f.days[0].temp_max));
  EXPECT_TRUE(std::isnan(f.days[0].temp_min));
  EXPECT_STREQ("", f.days[0].condition);
}

TEST(WeatherParse, UsesNanWhenHomeAssistantSendsUnavailableAsAString) {
  // HA remplace les valeurs numériques par "unavailable"/"unknown" quand
  // l'intégration météo décroche : cJSON_IsNumber l'écarte, pas de 0 parasite.
  const weather::Forecast f =
      parse(wrap("[{\"datetime\":\"2026-07-13T12:00:00+00:00\","
                 "\"temperature\":\"unavailable\",\"templow\":null}]"));
  ASSERT_EQ(1, f.count);
  EXPECT_TRUE(std::isnan(f.days[0].temp_max));
  EXPECT_TRUE(std::isnan(f.days[0].temp_min));
}

TEST(WeatherParse, TruncatesAnOverlongConditionAndKeepsItTerminated) {
  const std::string longue(80, 'c');
  const weather::Forecast f = parse(
      wrap("[{\"datetime\":\"2026-07-11T12:00:00+00:00\",\"condition\":\"" +
           longue + "\"}]"));
  ASSERT_EQ(1, f.count);
  EXPECT_EQ(sizeof(weather::DayForecast::condition) - 1,
            std::string(f.days[0].condition).size());
}

TEST(WeatherParse, CapsTheForecastAtSevenDays) {
  // days[7] : HA peut renvoyer 10 jours ou 24 créneaux horaires selon
  // l'intégration. Au-delà de 7 le parser écrirait hors du tableau.
  std::string arr = "[";
  for (int i = 0; i < 14; ++i) {
    if (i)
      arr += ",";
    arr += "{\"temperature\":" + std::to_string(10 + i) + "}";
  }
  arr += "]";
  const weather::Forecast f = parse(wrap(arr));
  EXPECT_EQ(7, f.count);
  EXPECT_NEAR(16.0f, f.days[6].temp_max, 0.01f); // le 7e, pas le 14e
}

// --- Conditions actuelles (GET /api/states) ---------------------------------

TEST(WeatherParse, ReadsCurrentConditions) {
  weather::CurrentConditions cc;
  ASSERT_TRUE(weather::parse_current_json(
      "{\"state\":\"partlycloudy\",\"attributes\":{\"temperature\":24.1,"
      "\"humidity\":62,\"pressure\":1013.2,\"wind_speed\":12.3}}",
      cc));
  EXPECT_TRUE(cc.valid);
  EXPECT_STREQ("partlycloudy", cc.condition);
  EXPECT_NEAR(24.1f, cc.temperature, 0.01f);
  EXPECT_NEAR(62.0f, cc.humidity, 0.01f);
  EXPECT_NEAR(1013.2f, cc.pressure, 0.01f);
  EXPECT_NEAR(12.3f, cc.wind_speed, 0.01f);
}

TEST(WeatherParse, RejectsMalformedCurrentJson) {
  weather::CurrentConditions cc;
  EXPECT_FALSE(weather::parse_current_json("<html>404</html>", cc));
  EXPECT_FALSE(cc.valid);
}

TEST(WeatherParse, MissingAttributesAreNanButStillValid) {
  // Champs absents = affichage partiel, pas un échec de fetch : le bandeau
  // garde la condition et masque les mesures manquantes.
  weather::CurrentConditions cc;
  ASSERT_TRUE(weather::parse_current_json("{\"state\":\"rainy\"}", cc));
  EXPECT_TRUE(cc.valid);
  EXPECT_STREQ("rainy", cc.condition);
  EXPECT_TRUE(std::isnan(cc.temperature));
  EXPECT_TRUE(std::isnan(cc.humidity));
  EXPECT_TRUE(std::isnan(cc.pressure));
  EXPECT_TRUE(std::isnan(cc.wind_speed));
}

TEST(WeatherParse, MissingStateLeavesTheConditionEmpty) {
  weather::CurrentConditions cc;
  ASSERT_TRUE(weather::parse_current_json(
      "{\"attributes\":{\"temperature\":20.0}}", cc));
  EXPECT_STREQ("", cc.condition);
  EXPECT_NEAR(20.0f, cc.temperature, 0.01f);
}

TEST(WeatherParse, UnavailableEntityYieldsNoNumbers) {
  // État réel d'une entité HS : state="unavailable", attributs absents.
  weather::CurrentConditions cc;
  ASSERT_TRUE(weather::parse_current_json("{\"state\":\"unavailable\"}", cc));
  EXPECT_STREQ("unavailable", cc.condition);
  EXPECT_TRUE(std::isnan(cc.temperature));
}

} // namespace
