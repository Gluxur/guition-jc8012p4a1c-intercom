#pragma once

namespace mqtt_ctrl {

// Transport MQTT SEUL : ce module ne connaît aucune entité. Le catalogue vit
// dans app/ha_entities, seule couche autorisée à connaître bsp, prefs,
// call_history et camera_session à la fois. Le mélange des deux avait produit
// cinq blocs snprintf codés en dur et une chaîne de else-if sur les topics —
// intenable à vingt-sept entités.
struct Callbacks {
  // Appelé pour CHAQUE message reçu, depuis la tâche esp-mqtt. Ni `topic` ni
  // `data` ne sont terminés par '\0' : c'est le contrat d'esp-mqtt, le routage
  // se fait par longueur explicite. `retained` = message rejoué par le broker
  // à l'abonnement (donc à chaque reconnexion), pas un envoi frais.
  void (*on_message)(const char *topic, int topic_len, const char *data,
                     int data_len, bool retained);
  // Appelé à chaque (re)connexion : le catalogue y republie sa découverte et y
  // repose ses abonnements.
  void (*on_connected)();
};

// À appeler une fois au boot, APRÈS l'IP et APRÈS ha_entities::init().
void init(const Callbacks &cbs);

// Publie un message. Renvoie false si le client n'est pas initialisé. Sûr
// depuis n'importe quelle tâche (esp_mqtt_client_publish est thread-safe).
bool publish(const char *topic, const char *payload, int len, int qos,
             bool retain);

// S'abonne. Appelable depuis on_connected() (le client est connecté à ce
// moment-là) comme depuis toute autre tâche.
bool subscribe(const char *topic, int qos);

// True entre MQTT_EVENT_CONNECTED et MQTT_EVENT_DISCONNECTED.
bool connected();

} // namespace mqtt_ctrl
