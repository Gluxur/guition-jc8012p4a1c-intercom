#include "mqtt_ctrl.h"

#include <atomic>
#include <string.h>

#include "esp_log.h"
#include "mqtt_client.h"

#include "app_config.h"
#include "sys/settings.h"

namespace mqtt_ctrl {
namespace {

constexpr const char *TAG = "mqtt_ctrl";

// Callbacks fournis par le catalogue. Stockés au niveau fichier car le
// gestionnaire d'événements esp-mqtt (signature C fixe) n'a pas de contexte
// typé pour les transporter proprement autrement.
Callbacks s_cbs = {};
esp_mqtt_client_handle_t s_client = nullptr;
std::atomic<bool> s_connected{false};

void event_handler(void *handler_args, esp_event_base_t base, int32_t event_id,
                   void *event_data) {
  (void)handler_args;
  (void)base;
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

  switch ((esp_mqtt_event_id_t)event_id) {
  case MQTT_EVENT_CONNECTED:
    s_connected.store(true);
    // Retenu : un abonné qui arrive après nous doit voir « online ». C'est le
    // pendant du testament, publié par le broker si nous tombons.
    esp_mqtt_client_publish(s_client, cfg::topic::kStatus,
                            cfg::topic::kStatusOnline,
                            (int)strlen(cfg::topic::kStatusOnline), 1, 1);
    ESP_LOGI(TAG, "connecté au broker");
    if (s_cbs.on_connected) {
      s_cbs.on_connected();
    }
    break;
  case MQTT_EVENT_DISCONNECTED:
    s_connected.store(false);
    ESP_LOGI(TAG, "déconnecté (reconnexion auto esp-mqtt)");
    break;
  case MQTT_EVENT_DATA:
    // Un message plus grand que le tampon arrive en plusieurs événements : les
    // fragments étaient traités comme autant de messages complets (une URL OTA
    // tronquée, par exemple). Aucune de nos commandes n'approche la taille du
    // tampon : on jette plutôt que de réassembler.
    if (event->current_data_offset != 0 ||
        event->data_len != event->total_data_len) {
      ESP_LOGW(TAG, "message fragmenté ignoré (%d o)", event->total_data_len);
      break;
    }
    if (s_cbs.on_message) {
      s_cbs.on_message(event->topic, event->topic_len, event->data,
                       event->data_len, event->retain);
    }
    break;
  default:
    break;
  }
}

} // namespace

void init(const Callbacks &cbs) {
  s_cbs = cbs;

  // Broker et identifiants viennent de settings (NVS ou défauts compilés) :
  // esp-mqtt ne copie pas ces chaînes, elles doivent rester valides — c'est le
  // cas, settings::get() renvoie une référence stable pour toute la vie du
  // programme.
  esp_mqtt_client_config_t mqtt_cfg = {};
  mqtt_cfg.broker.address.uri = settings::mqtt_broker_uri();
  mqtt_cfg.credentials.username = settings::get().mqtt_user;
  mqtt_cfg.credentials.authentication.password = settings::get().mqtt_pass;

  // Testament : publié par le BROKER si notre connexion tombe sans DISCONNECT
  // propre. C'est la seule façon pour Home Assistant de distinguer « valeur
  // ancienne » de « appareil absent ».
  mqtt_cfg.session.last_will.topic = cfg::topic::kStatus;
  mqtt_cfg.session.last_will.msg = cfg::topic::kStatusOffline;
  mqtt_cfg.session.last_will.msg_len = (int)strlen(cfg::topic::kStatusOffline);
  mqtt_cfg.session.last_will.qos = 1;
  mqtt_cfg.session.last_will.retain = 1;

  s_client = esp_mqtt_client_init(&mqtt_cfg);
  if (s_client == nullptr) {
    ESP_LOGE(TAG, "esp_mqtt_client_init a échoué (feature MQTT désactivée)");
    return;
  }

  esp_err_t err = esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY,
                                                 event_handler, nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "register_event -> %s", esp_err_to_name(err));
    esp_mqtt_client_destroy(s_client);
    s_client = nullptr;
    return;
  }

  err = esp_mqtt_client_start(s_client);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_mqtt_client_start -> %s", esp_err_to_name(err));
    esp_mqtt_client_destroy(s_client);
    s_client = nullptr;
    return;
  }
  ESP_LOGI(TAG, "client MQTT démarré (%s)", settings::mqtt_broker_uri());
}

bool publish(const char *topic, const char *payload, int len, int qos,
             bool retain) {
  if (s_client == nullptr)
    return false;
  int id = esp_mqtt_client_publish(s_client, topic, payload, len, qos,
                                   retain ? 1 : 0);
  return id >= 0;
}

bool subscribe(const char *topic, int qos) {
  if (s_client == nullptr)
    return false;
  return esp_mqtt_client_subscribe(s_client, topic, qos) >= 0;
}

bool connected() { return s_connected.load(); }

} // namespace mqtt_ctrl
