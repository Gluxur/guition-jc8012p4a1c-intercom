// Pure decoding of the calendar.get_events action response, reshaped by the
// template of gcal.cpp into {"response": [<event>, ...]} where each event has
// "summary" and "start"/"end" as {"dateTime": ...} or {"date": ...} (the
// layout of HA's REST calendar API, which this parser was written for).
// Split from gcal.cpp (API call, FreeRTOS task, cache under mutex) so the PC
// compiler can test it: test/test_gcal_parse.cpp.

#include "net/gcal.h"

#include "cJSON.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>

namespace gcal {

static const char *TAG = "gcal";

// Howard Hinnant, "chrono-compatible low-level date algorithms" (même
// famille que weather::weekday_from_civil) : jours depuis l'époque civile.
// `timegm` n'existe pas dans le newlib de ce build ESP-IDF — cet algorithme
// est portable et ne dépend d'aucune fonction de fuseau horaire de la libc.
static int64_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy =
      (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2) / 5 +
      static_cast<unsigned>(d) - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097LL + static_cast<int64_t>(doe) - 719468;
}

static int64_t epoch_utc(int y, int mo, int d, int h, int mi, int se) {
  return days_from_civil(y, mo, d) * 86400LL + h * 3600LL + mi * 60LL + se;
}

// RFC3339 -> epoch UTC. `has_time`=true : "YYYY-MM-DDTHH:MM:SS[+HH:MM|-HH:MM|Z]"
// (événement horaire). `has_time`=false : "YYYY-MM-DD" (journée entière,
// converti en minuit UTC — sert uniquement au regroupement par jour côté UI).
static bool parse_rfc3339(const char *s, bool has_time, int64_t &epoch_out) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
  if (has_time) {
    char tz_sign = 0;
    int tz_h = 0, tz_mi = 0;
    int n = sscanf(s, "%d-%d-%dT%d:%d:%d%c%d:%d", &y, &mo, &d, &h, &mi, &se,
                   &tz_sign, &tz_h, &tz_mi);
    if (n < 6)
      return false;
    int64_t naive_utc = epoch_utc(y, mo, d, h, mi, se);
    int64_t offset_s = 0;
    if (n >= 9 && (tz_sign == '+' || tz_sign == '-')) {
      offset_s =
          (int64_t)(tz_h * 3600 + tz_mi * 60) * (tz_sign == '-' ? -1 : 1);
    }
    epoch_out = naive_utc - offset_s;
    return true;
  }
  if (sscanf(s, "%d-%d-%d", &y, &mo, &d) != 3)
    return false;
  epoch_out = epoch_utc(y, mo, d, 0, 0, 0);
  return true;
}

bool parse_events_json(const char *json, EventList &out) {
  out = EventList{};
  cJSON *doc = cJSON_Parse(json);
  if (!doc) {
    ESP_LOGE(TAG, "JSON invalide");
    return false;
  }
  cJSON *root = cJSON_GetObjectItem(doc, "response");
  if (!cJSON_IsArray(root)) {
    ESP_LOGE(TAG, "response absent ou pas un tableau");
    cJSON_Delete(doc);
    return false;
  }
  int count = 0;
  cJSON *e = nullptr;
  cJSON_ArrayForEach(e, root) {
    if (count >= EventList::kMax)
      break;
    Event &ev = out.events[count];
    ev = Event{};
    cJSON *summary = cJSON_GetObjectItem(e, "summary");
    if (cJSON_IsString(summary))
      strlcpy(ev.summary, summary->valuestring, sizeof ev.summary);

    cJSON *start = cJSON_GetObjectItem(e, "start");
    cJSON *end = cJSON_GetObjectItem(e, "end");
    if (!start)
      continue; // événement sans borne de début : ignoré

    cJSON *sdt = cJSON_GetObjectItem(start, "dateTime");
    cJSON *sd = cJSON_GetObjectItem(start, "date");
    bool ok_start = false;
    if (cJSON_IsString(sdt)) {
      ev.all_day = false;
      ok_start = parse_rfc3339(sdt->valuestring, true, ev.start_epoch_s);
    } else if (cJSON_IsString(sd)) {
      ev.all_day = true;
      ok_start = parse_rfc3339(sd->valuestring, false, ev.start_epoch_s);
    }
    if (!ok_start)
      continue;

    ev.end_epoch_s = ev.start_epoch_s;
    if (end) {
      cJSON *edt = cJSON_GetObjectItem(end, "dateTime");
      cJSON *ed = cJSON_GetObjectItem(end, "date");
      int64_t end_epoch = 0;
      if (cJSON_IsString(edt) &&
          parse_rfc3339(edt->valuestring, true, end_epoch))
        ev.end_epoch_s = end_epoch;
      else if (cJSON_IsString(ed) &&
               parse_rfc3339(ed->valuestring, false, end_epoch))
        ev.end_epoch_s = end_epoch;
    }
    count++;
  }
  out.count = count;

  // Tri par insertion (count <= kMax = 48, coût négligeable) : l'ordre renvoyé
  // par l'intégration Google Calendar de HA n'est pas garanti.
  for (int i = 1; i < out.count; ++i) {
    Event key = out.events[i];
    int j = i - 1;
    while (j >= 0 && out.events[j].start_epoch_s > key.start_epoch_s) {
      out.events[j + 1] = out.events[j];
      j--;
    }
    out.events[j + 1] = key;
  }

  cJSON_Delete(doc);
  return true;
}

int64_t utc_midnight(int y, int mo, int d) {
  return days_from_civil(y, mo, d) * 86400LL;
}

// Fenêtre du jour applicable à `e` + fin effective. Deux repères de temps
// cohabitent : les journées entières n'ont qu'une date dans le JSON et sont
// ancrées à minuit UTC, les horaires sont des instants réels comparables à la
// fenêtre locale. Cf. le commentaire de events_on_day() dans gcal.h.
static void day_window_for(const Event &e, int64_t day_start_s,
                           int64_t day_end_s, int64_t utc_day_start_s,
                           int64_t &w0, int64_t &w1, int64_t &end_eff) {
  if (e.all_day) {
    w0 = utc_day_start_s;
    w1 = utc_day_start_s + 86400;
    // "end" absent ou mal formé : le parseur a recopié le début. Une journée
    // entière dure au moins son jour, sinon elle ne chevaucherait rien.
    end_eff = (e.end_epoch_s > e.start_epoch_s) ? e.end_epoch_s
                                                : e.start_epoch_s + 86400;
    return;
  }
  w0 = day_start_s;
  w1 = day_end_s;
  // Durée nulle (pas de "end") : traité comme un instant, sinon un événement
  // tombant pile sur la borne basse serait rejeté par le test strict.
  end_eff = (e.end_epoch_s > e.start_epoch_s) ? e.end_epoch_s
                                              : e.start_epoch_s + 1;
}

int events_on_day(const EventList &l, int64_t day_start_s, int64_t day_end_s,
                  int64_t utc_day_start_s, const Event **out, int max_out) {
  int n = 0;
  for (int i = 0; i < l.count && n < max_out; ++i) {
    const Event &e = l.events[i];
    int64_t w0, w1, end_eff;
    day_window_for(e, day_start_s, day_end_s, utc_day_start_s, w0, w1, end_eff);
    // Chevauchement à bornes semi-ouvertes : un événement finissant à minuit
    // pile n'appartient pas au jour qui commence, sinon il s'affiche sur deux
    // jours consécutifs.
    if (e.start_epoch_s >= w1 || end_eff <= w0)
      continue;
    out[n++] = &e;
  }
  return n;
}

bool starts_before_day(const Event &e, int64_t day_start_s,
                       int64_t utc_day_start_s) {
  return e.start_epoch_s < (e.all_day ? utc_day_start_s : day_start_s);
}

bool range_covers(const EventList &l, int64_t start_s, int64_t end_s,
                  int64_t now_us, int64_t max_age_us) {
  // `valid` d'abord : sans fetch réussi les bornes valent 0 et engloberaient
  // n'importe quelle fenêtre du passé par simple inégalité.
  if (!l.valid)
    return false;
  if (l.range_start_s > start_s || l.range_end_s < end_s)
    return false;
  return (now_us - l.fetched_at_us) <= max_age_us;
}

} // namespace gcal
