#include "ha_discovery_fmt.h"

#include <cstdio>

namespace ha_fmt {
namespace {

// Fragment `dev` commun : regroupe toutes les entités sous un seul appareil
// Home Assistant. Identique à celui publié avant le découpage du catalogue —
// le changer créerait un second appareil et orphelinerait les entités déjà
// adoptées par l'utilisateur.
constexpr const char *kDev =
    "\"dev\":{\"ids\":[\"visiophone_p4\"],\"name\":\"Visiophone P4\","
    "\"mf\":\"ESP32-P4\",\"mdl\":\"go2rtc\"}";

} // namespace

int written(int n, size_t cap) {
  if (n < 0) {
    return 0;
  }
  return n < static_cast<int>(cap) ? n : static_cast<int>(cap) - 1;
}

int discovery_topic(char *out, size_t cap, const EntityDesc &e) {
  return written(snprintf(out, cap, "%s/%s/%s/%s/config", kDiscoveryPrefix,
                          e.component, kDeviceSlug, e.object_id),
                 cap);
}

int default_state_topic(char *out, size_t cap, const char *object_id) {
  return written(snprintf(out, cap, "%s/%s/state", kEntityPrefix, object_id),
                 cap);
}

int default_command_topic(char *out, size_t cap, const char *object_id) {
  return written(snprintf(out, cap, "%s/%s/set", kEntityPrefix, object_id),
                 cap);
}

int discovery_payload(char *out, size_t cap, const EntityDesc &e) {
  char stat[128] = {0};
  if (e.state_topic != nullptr) {
    snprintf(stat, sizeof stat, ",\"stat_t\":\"%s\"", e.state_topic);
  }

  char cmd[128] = {0};
  if (e.command_topic != nullptr) {
    snprintf(cmd, sizeof cmd, ",\"cmd_t\":\"%s\"", e.command_topic);
  }

  char extra[256] = {0};
  if (e.extra != nullptr && e.extra[0] != '\0') {
    snprintf(extra, sizeof extra, ",%s", e.extra);
  }

  const int n = snprintf(out, cap,
                         "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\","
                         "\"avty_t\":\"%s\"%s%s%s,%s}",
                         e.name, kDeviceSlug, e.object_id, kStatusTopic, stat,
                         cmd, extra, kDev);
  return written(n, cap);
}

} // namespace ha_fmt
