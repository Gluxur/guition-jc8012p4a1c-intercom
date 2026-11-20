#pragma once

#include <cstddef>

// Formatage des payloads MQTT Discovery de Home Assistant.
//
// Module PUR : aucun header ESP-IDF, aucune IO, aucun état. C'est ce qui le
// rend testable sur PC — et ce formatage MÉRITE des tests : snprintf renvoie
// la longueur qu'il AURAIT écrite, et passer ce retour tel quel à
// esp_mqtt_client_publish publie au-delà du tampon. Ce piège a déjà été
// rencontré dans ce projet.
namespace ha_fmt {

// Préfixes. Dupliqués ici plutôt qu'inclus depuis app_config.h : ce module ne
// dépend de rien. C'est app/ha_entities qui fait le pont, et qui vérifie au
// boot que les deux copies du topic de disponibilité concordent.
inline constexpr const char *kEntityPrefix = "visio";
inline constexpr const char *kDiscoveryPrefix = "homeassistant";
inline constexpr const char *kDeviceSlug = "visiophone";
inline constexpr const char *kStatusTopic = "visio/status";

struct EntityDesc {
  const char *component;     // "sensor", "switch", "number", "button", "light"
  const char *object_id;     // unique dans l'appareil
  const char *name;          // libellé affiché dans Home Assistant
  const char *extra;         // fragment JSON propre à l'entité, sans accolades
                             // englobantes ni virgule de tête. "" si aucun.
  const char *state_topic;   // nullptr => pas de stat_t dans le payload
  const char *command_topic; // nullptr => pas de cmd_t
};

// Borne le retour de snprintf au contenu RÉELLEMENT écrit. Renvoie 0 si n < 0.
[[nodiscard]] int written(int n, size_t cap);

// "homeassistant/<component>/visiophone/<object_id>/config"
int discovery_topic(char *out, size_t cap, const EntityDesc &e);

// "visio/<object_id>/state" et "visio/<object_id>/set"
int default_state_topic(char *out, size_t cap, const char *object_id);
int default_command_topic(char *out, size_t cap, const char *object_id);

// Payload complet de découverte, avec le fragment `dev` commun, `avty_t`,
// `uniq_id`, et `stat_t`/`cmd_t` selon ce que l'entité déclare.
// Renvoie la longueur à publier (déjà bornée par written()).
int discovery_payload(char *out, size_t cap, const EntityDesc &e);

} // namespace ha_fmt
