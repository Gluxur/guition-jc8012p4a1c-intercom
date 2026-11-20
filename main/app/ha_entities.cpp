#include "ha_entities.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h" // xTaskCreatePinnedToCoreWithCaps
#include "freertos/task.h"

#include "app/call_history.h"
#include "app/camera_session.h"
#include "app/ha_discovery_fmt.h"
#include "app/presence.h"
#include "app_config.h"
#include "bsp.h"
#include "media/ringtone.h"
#include "net/file_server.h"
#include "net/wifi_net.h"
#include "sys/log_stream.h"
#include "sys/mqtt_ctrl.h"
#include "sys/ota_ctrl.h"
#include "sys/prefs.h"
#include "sys/sd_log.h"
#include "sys/settings.h"
#include "sys/sys_stats.h"
#include "sys/usb_msc.h"
#include "ui/ui.h"
#include "ui/ui_camera_v2.h"

namespace ha_entities {
namespace {

constexpr const char *TAG = "ha_ent";

// ha_fmt::kStatusTopic duplique cfg::topic::kStatus (le module de formatage est
// pur et ne dépend de rien). Rien ne se vérifie en constexpr sur des pointeurs
// de chaîne : la cohérence est contrôlée une fois au boot, dans init().

Hooks s_hooks = {};
mqtt_ctrl::Callbacks s_mqtt_cbs = {};

// Entité du catalogue. Une seule table, parcourue trois fois : découverte,
// abonnements, publication d'états.
struct Entity {
  const char *component;
  const char *object_id;
  const char *name;
  const char *extra;
  const char *state_topic;   // nullptr => "visio/<object_id>/state"
  const char *command_topic; // nullptr => pas de commande
  void (*get)(char *out, size_t cap);     // nullptr pour un button
  void (*set)(const char *data, int len); // nullptr pour un capteur
  bool sd_touching; // get() lit la carte -> sauté pendant une session
};

// --- Getters ---------------------------------------------------------------

void g_bool(char *out, size_t cap, bool v) {
  snprintf(out, cap, "%s", v ? "ON" : "OFF");
}

void g_dnd(char *o, size_t c) { g_bool(o, c, prefs::dnd()); }
void g_stats(char *o, size_t c) { g_bool(o, c, prefs::show_stats()); }
void g_sdlog(char *o, size_t c) { g_bool(o, c, sd_log::enabled()); }
void g_filesrv(char *o, size_t c) { g_bool(o, c, file_server::running()); }
void g_call(char *o, size_t c) { g_bool(o, c, camera_session::active()); }

void g_presence(char *o, size_t c) { g_bool(o, c, presence::present()); }

void g_ring_volume(char *o, size_t c) {
  snprintf(o, c, "%u", (unsigned)prefs::ring_volume());
}

// Schéma JSON : un seul topic pour l'allumage ET la luminosité. Le schéma par
// défaut de Home Assistant en aurait exigé quatre.
// L'état allumé/éteint vient du BSP (display_on) et non de backlight(), qui
// renvoie le réglage MÉMORISÉ — une extinction le laisse intact exprès. La
// luminosité rapportée reste ce réglage : pendant un appel le panneau est
// poussé à 100 %, mais ce n'est pas ce que l'utilisateur a choisi.
void g_screen(char *o, size_t c) {
  const bsp::Bsp &bsp_inst = bsp::Bsp::instance();
  snprintf(o, c, "{\"state\":\"%s\",\"brightness\":%u}",
           bsp_inst.display_on() ? "ON" : "OFF",
           (unsigned)bsp_inst.backlight());
}

// Libellés de l'entité select, dans l'ordre de prefs::EchoCancel : l'index EST
// le réglage, comme pour le dropdown de la page Configuration.
constexpr const char *kEchoLabels[] = {"Desactivee", "Economique",
                                       "Haute performance"};

void g_echo(char *o, size_t c) {
  snprintf(o, c, "%s", kEchoLabels[static_cast<int>(prefs::echo_cancel())]);
}

void g_page(char *o, size_t c) { snprintf(o, c, "%s", ui::current_page()); }

void g_rssi(char *o, size_t c) { snprintf(o, c, "%d", wifi_net::wifi_rssi()); }

void g_ip(char *o, size_t c) {
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip = {};
  if (netif == nullptr || esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
    snprintf(o, c, "0.0.0.0");
    return;
  }
  snprintf(o, c, IPSTR, IP2STR(&ip.ip));
}

void g_uptime(char *o, size_t c) {
  snprintf(o, c, "%lld", (long long)(esp_timer_get_time() / 1000000));
}

void g_psram(char *o, size_t c) {
  snprintf(o, c, "%d", sys_stats::psram_used_percent());
}

void g_sram_free(char *o, size_t c) {
  snprintf(o, c, "%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void g_cpu0(char *o, size_t c) {
  snprintf(o, c, "%d", sys_stats::cpu_usage_percent().core_a);
}

void g_cpu1(char *o, size_t c) {
  snprintf(o, c, "%d", sys_stats::cpu_usage_percent().core_b);
}

// sd_capacity() lit la carte : cette entité porte sd_touching = true et n'est
// donc jamais interrogée pendant une session caméra.
void g_sd_free(char *o, size_t c) {
  uint64_t total = 0, freeb = 0;
  if (!bsp::Bsp::instance().sd_capacity(&total, &freeb)) {
    snprintf(o, c, "0");
    return;
  }
  snprintf(o, c, "%llu", (unsigned long long)(freeb / (1024 * 1024)));
}

void g_missed(char *o, size_t c) {
  snprintf(o, c, "%lu", (unsigned long)call_history::missed_count());
}

void g_fw(char *o, size_t c) {
  const esp_app_desc_t *d = esp_app_get_description();
  snprintf(o, c, "%s", d ? d->version : "unknown");
}

// --- Setters ---------------------------------------------------------------

bool payload_on(const char *d, int len) {
  if (len >= 2 && (d[0] == 'O' || d[0] == 'o') && (d[1] == 'N' || d[1] == 'n')) {
    return true;
  }
  return len >= 1 && d[0] == '1';
}

void s_dnd_set(const char *d, int len) { prefs::set_dnd(payload_on(d, len)); }

void s_stats_set(const char *d, int len) {
  prefs::set_show_stats(payload_on(d, len));
}

// Même paire « appliquer puis persister » que cb_sd_log de la page
// Configuration : prefs ne pilote aucun module.
void s_sdlog_set(const char *d, int len) {
  const bool on = payload_on(d, len);
  sd_log::set_enabled(on);
  prefs::set_sd_log_enabled(on);
}

void s_filesrv_set(const char *d, int len) {
  const bool on = payload_on(d, len);
  if (on) {
    if (!file_server::start()) {
      return; // l'écho republiera l'état réel : l'entité ne mentira pas
    }
  } else {
    file_server::stop();
  }
  prefs::set_file_server(on);
}

void s_loglive_set(const char *d, int len) {
  log_stream::set_enabled(payload_on(d, len));
}

void g_speaker_volume(char *o, size_t c) {
  snprintf(o, c, "%u", (unsigned)bsp::Bsp::instance().speaker_volume());
}

void s_speaker_volume_set(const char *d, int len) {
  char buf[8];
  const int n = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
  memcpy(buf, d, n);
  buf[n] = '\0';
  const int v = atoi(buf);
  if (v < 0 || v > 100) {
    return;
  }
  bsp::Bsp::instance().set_speaker_volume((uint8_t)v);
}

void g_mic_gain(char *o, size_t c) {
  snprintf(o, c, "%u", (unsigned)bsp::Bsp::instance().mic_gain());
}

// Réglable à distance : un gain trop haut sature l'ADC aux attaques de mots,
// et le G.711 transforme l'écrêtage en grésillement côté rue — c'est un
// réglage qu'on ajuste entre deux coups de sonnette, pas depuis la tablette.
void s_mic_gain_set(const char *d, int len) {
  char buf[8];
  const int n = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
  memcpy(buf, d, n);
  buf[n] = '\0';
  const int v = atoi(buf);
  if (v < 0 || v > 100) {
    return;
  }
  bsp::Bsp::instance().set_mic_gain((uint8_t)v);
}

// Ouverture « à la main », comme le bouton Caméra de l'accueil : ni sonnerie
// intérieure, ni ding-dong côté rue, ni trace dans le journal d'appels. C'est
// ce qui permet de mesurer l'ouverture à distance (et la nuit) sans réveiller
// la rue ni fabriquer de faux appels manqués — `visiophone/ring` fait tout ça.
void s_camera_set(const char *d, int len) {
  const bool on = len == 2 && memcmp(d, "ON", 2) == 0;
  if (on) {
    if (camera_session::active() ||
        usb_msc::status() == usb_msc::Status::kActive) {
      return;
    }
    ui_camera_v2::open_screen();
  } else if (camera_session::active()) {
    camera_session::stop();
  }
}

void s_ring_volume_set(const char *d, int len) {
  char buf[8];
  const int n = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
  memcpy(buf, d, n);
  buf[n] = '\0';
  const int v = atoi(buf);
  // Hors bornes : ignoré. L'écho republie la valeur réelle et Home Assistant
  // revient tout seul au bon affichage.
  if (v < 0 || v > 100) {
    return;
  }
  prefs::set_ring_volume((uint8_t)v);
  ringtone::set_volume((uint8_t)v);
}

// Payload attendu : {"state":"ON"} ou {"state":"ON","brightness":70}. Analyse
// par recherche de sous-chaîne plutôt que cJSON : trois champs possibles, et
// cette fonction s'exécute dans la tâche esp-mqtt, dont la pile est modeste.
void s_screen_set(const char *d, int len) {
  char buf[96];
  const int n = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
  memcpy(buf, d, n);
  buf[n] = '\0';

  const char *b = strstr(buf, "\"brightness\"");
  if (b != nullptr) {
    const char *colon = strchr(b, ':');
    if (colon != nullptr) {
      const int v = atoi(colon + 1);
      // ⚠ 0 ne passe PAS par set_backlight : celui-ci persiste la valeur en
      // NVS, et l'appareil redémarrerait écran noir, sans plus aucun moyen de
      // le réparer depuis l'écran (cf. le commentaire de Bsp::backlight_off).
      // Une extinction est un état transitoire, pas un réglage.
      if (v == 0) {
        bsp::Bsp::instance().backlight_off();
      } else if (v > 0 && v <= 100) {
        bsp::Bsp::instance().set_backlight((uint8_t)v);
      }
    }
  }
  if (strstr(buf, "\"state\":\"OFF\"") != nullptr) {
    bsp::Bsp::instance().backlight_off();
  } else if (strstr(buf, "\"state\":\"ON\"") != nullptr && b == nullptr) {
    // Rallumage sans luminosité précisée : on revient au réglage mémorisé.
    bsp::Bsp::instance().restore_backlight();
  }
}

// Libellé inconnu : ignoré, l'écho republie la valeur réelle.
void s_echo_set(const char *d, int len) {
  for (size_t i = 0; i < sizeof(kEchoLabels) / sizeof(kEchoLabels[0]); i++) {
    if (len == (int)strlen(kEchoLabels[i]) &&
        memcmp(d, kEchoLabels[i], len) == 0) {
      prefs::set_echo_cancel(static_cast<prefs::EchoCancel>(i));
      return;
    }
  }
}

void s_reboot(const char *, int) {
  ESP_LOGI(TAG, "redémarrage demandé par MQTT");
  sd_log::flush_now();
  esp_restart();
}

void s_presence_snapshot(const char *, int) { presence::request_snapshot(); }

// Navigation à distance (mesures de charge par écran, pilotage depuis HA).
// Refusée pendant une session caméra (direct_mode : l'écran vidéo possède le
// framebuffer, une page LVGL chargée dessous resterait invisible) et en mode
// clé USB (son écran dédié porte la sortie du mode).
void s_page_set(const char *d, int len) {
  if (camera_session::active() ||
      usb_msc::status() == usb_msc::Status::kActive) {
    return;
  }
  char name[24];
  const int n = len < (int)sizeof(name) - 1 ? len : (int)sizeof(name) - 1;
  memcpy(name, d, n);
  name[n] = '\0';
  if (!ui::open_page(name)) {
    ESP_LOGW(TAG, "page « %s » : inconnue ou LVGL indisponible", name);
    return;
  }
  // L'ouverture est postée à lvgl_task : lui laisser un tour avant l'écho que
  // on_message publie juste après, sinon HA recevrait l'ancienne page.
  vTaskDelay(pdMS_TO_TICKS(300));
}

void s_ring(const char *, int) {
  if (s_hooks.ring) {
    s_hooks.ring();
  }
}

void g_ota_status(char *o, size_t c) { ota_ctrl::status(o, c); }

void s_ota_url(const char *d, int len) {
  char url[256];
  const int n = len < (int)sizeof(url) - 1 ? len : (int)sizeof(url) - 1;
  memcpy(url, d, n);
  url[n] = '\0';
  // Seul l'hôte configuré (Home Assistant / go2rtc / broker) peut servir un
  // firmware : sans ce filtre, quiconque détient les identifiants MQTT —
  // transmis en clair — faisait flasher une image venue de n'importe où. La
  // comparaison inclut le séparateur qui suit l'hôte, pour qu'un
  // « 192.168.1.10.evil » ne passe pas pour « 192.168.1.10 ».
  const char *host = settings::get().host;
  const size_t hl = strlen(host);
  bool allowed = false;
  static constexpr const char *kSchemes[] = {"http://", "https://"};
  for (const char *scheme : kSchemes) {
    const size_t sl = strlen(scheme);
    if (strncmp(url, scheme, sl) == 0 && strncmp(url + sl, host, hl) == 0) {
      const char next = url[sl + hl];
      allowed = next == ':' || next == '/' || next == '\0';
    }
  }
  if (!allowed) {
    ESP_LOGW(TAG, "OTA refusée : URL hors de l'hôte configuré (%s)", host);
    static constexpr const char kMsg[] = "fail: url not allowed";
    mqtt_ctrl::publish(cfg::topic::kOtaStatus, kMsg, (int)sizeof(kMsg) - 1, 1,
                       false);
    return;
  }
  ota_ctrl::request(url);
}

void s_ota_sd(const char *d, int len) {
  char path[128];
  const int n = len < (int)sizeof(path) - 1 ? len : (int)sizeof(path) - 1;
  memcpy(path, d, n);
  path[n] = '\0';
  // Payload qui n'est pas un chemin absolu (bouton HA sans charge utile
  // utile) => chemin par défaut.
  ota_ctrl::request_file(path[0] == '/' ? path : cfg::topic::kOtaSdDefaultPath);
}

void s_ack_missed(const char *, int) { call_history::acknowledge_missed(); }

// --- Table -----------------------------------------------------------------

constexpr Entity kEntities[] = {
    // --- Réglages pilotables -------------------------------------------
    {"light", "screen", "Ecran",
     "\"schema\":\"json\",\"brightness\":true,\"bri_scl\":100", nullptr,
     "visio/screen/set", g_screen, s_screen_set, false},
    {"switch", "dnd", "Ne pas deranger", "\"ic\":\"mdi:bell-off\"", nullptr,
     "visio/dnd/set", g_dnd, s_dnd_set, false},
    // Pris en compte au décroché suivant (cf. prefs::echo_cancel).
    {"select", "page", "Page affichee",
     "\"options\":[\"accueil\",\"meteo\",\"agenda\",\"appels\","
     "\"configuration\",\"autre\"],\"ic\":\"mdi:monitor\"",
     nullptr, "visio/page/set", g_page, s_page_set, false},
    {"select", "echo_cancel", "Annulation d'echo",
     "\"options\":[\"Desactivee\",\"Economique\",\"Haute performance\"],"
     "\"ic\":\"mdi:account-voice\"",
     nullptr, "visio/echo_cancel/set", g_echo, s_echo_set, false},
    {"number", "ring_volume", "Volume sonnerie",
     "\"min\":0,\"max\":100,\"step\":5,\"ic\":\"mdi:volume-high\"", nullptr,
     "visio/ring_volume/set", g_ring_volume, s_ring_volume_set, false},
    {"number", "speaker_volume", "Volume haut-parleur",
     "\"min\":0,\"max\":100,\"step\":5,\"ic\":\"mdi:speaker\"", nullptr,
     "visio/speaker_volume/set", g_speaker_volume, s_speaker_volume_set, false},
    {"number", "mic_gain", "Gain micro",
     "\"min\":0,\"max\":100,\"step\":5,\"ic\":\"mdi:microphone\"", nullptr,
     "visio/mic_gain/set", g_mic_gain, s_mic_gain_set, false},
    {"switch", "camera", "Camera sans sonnerie",
     "\"ent_cat\":\"config\",\"ic\":\"mdi:cctv\"", nullptr,
     "visio/camera/set", g_call, s_camera_set, false},
    {"switch", "show_stats", "Overlay statistiques", "\"ent_cat\":\"config\"",
     nullptr, "visio/show_stats/set", g_stats, s_stats_set, false},
    {"switch", "sd_log", "Journal carte SD", "\"ent_cat\":\"config\"", nullptr,
     "visio/sd_log/set", g_sdlog, s_sdlog_set, false},
    {"switch", "file_server", "Serveur de fichiers",
     "\"ent_cat\":\"config\",\"ic\":\"mdi:folder-network\"", nullptr,
     "visio/file_server/set", g_filesrv, s_filesrv_set, false},
    // Topic historique conservé : des automatisations en dépendent.
    {"switch", "live_logs", "Logs live", "\"ent_cat\":\"config\"", nullptr,
     cfg::topic::kLogEnable, nullptr, s_loglive_set, false},

    // --- Actions --------------------------------------------------------
    {"button", "reboot", "Redemarrer",
     "\"pl_prs\":\"PRESS\",\"dev_cla\":\"restart\",\"ent_cat\":\"config\"",
     nullptr, "visio/reboot/set", nullptr, s_reboot, false},
    {"button", "open_camera", "Ouvrir l'ecran camera",
     "\"pl_prs\":\"PRESS\",\"ic\":\"mdi:video\"", nullptr,
     "visio/open_camera/set", nullptr, s_ring, false},
    {"button", "ack_missed", "Acquitter les appels manques",
     "\"pl_prs\":\"PRESS\",\"ic\":\"mdi:phone-check\"", nullptr,
     "visio/ack_missed/set", nullptr, s_ack_missed, false},
    {"button", "ota_sd", "Mettre a jour depuis la carte SD",
     "\"pl_prs\":\"/sdcard/fw.bin\",\"ent_cat\":\"config\"", nullptr,
     cfg::topic::kOtaSdCmd, nullptr, s_ota_sd, false},
    // Topic historique : le payload du bouton reste l'URL dérivée de l'hôte,
    // publiée par la découverte côté Home Assistant.
    {"button", "ota_update", "Mettre a jour (Wi-Fi)", "\"ent_cat\":\"config\"",
     nullptr, cfg::topic::kOtaCmd, nullptr, s_ota_url, false},

    // --- Capteurs -------------------------------------------------------
    {"sensor", "rssi", "RSSI Wi-Fi",
     "\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\","
     "\"ent_cat\":\"diagnostic\"",
     nullptr, nullptr, g_rssi, nullptr, false},
    {"sensor", "ip", "Adresse IP",
     "\"ic\":\"mdi:ip-network\",\"ent_cat\":\"diagnostic\"", nullptr, nullptr,
     g_ip, nullptr, false},
    {"sensor", "uptime", "Duree de fonctionnement",
     "\"unit_of_meas\":\"s\",\"dev_cla\":\"duration\","
     "\"ent_cat\":\"diagnostic\"",
     nullptr, nullptr, g_uptime, nullptr, false},
    {"sensor", "psram", "PSRAM utilisee",
     "\"unit_of_meas\":\"%\",\"ent_cat\":\"diagnostic\"", nullptr, nullptr,
     g_psram, nullptr, false},
    {"sensor", "sram_free", "SRAM interne libre",
     "\"unit_of_meas\":\"B\",\"dev_cla\":\"data_size\","
     "\"ent_cat\":\"diagnostic\"",
     nullptr, nullptr, g_sram_free, nullptr, false},
    {"sensor", "cpu0", "Charge CPU coeur 0",
     "\"unit_of_meas\":\"%\",\"ent_cat\":\"diagnostic\"", nullptr, nullptr,
     g_cpu0, nullptr, false},
    {"sensor", "cpu1", "Charge CPU coeur 1",
     "\"unit_of_meas\":\"%\",\"ent_cat\":\"diagnostic\"", nullptr, nullptr,
     g_cpu1, nullptr, false},
    {"sensor", "sd_free", "Espace libre carte SD",
     "\"unit_of_meas\":\"MB\",\"ent_cat\":\"diagnostic\"", nullptr, nullptr,
     g_sd_free, nullptr, true},
    // Non diagnostic : c'est une information d'usage, pas de maintenance.
    {"sensor", "missed_calls", "Appels manques", "\"ic\":\"mdi:phone-missed\"",
     nullptr, nullptr, g_missed, nullptr, false},
    {"sensor", "fw_version", "Version du firmware", "\"ent_cat\":\"diagnostic\"",
     cfg::topic::kFwVersion, nullptr, g_fw, nullptr, false},
    // Poussé en direct par ota_ctrl (report_cb de main.cpp) pour la
    // progression, ET relu ici : sans getter, l'entité restait figée sur
    // "reboot" — dernier message avant esp_restart(), que plus rien ne
    // remplaçait au démarrage suivant.
    {"sensor", "ota_status", "Statut OTA", "\"ent_cat\":\"diagnostic\"",
     cfg::topic::kOtaStatus, nullptr, g_ota_status, nullptr, false},
    // Logs : l'état est l'horodatage (change à chaque publication), le texte
    // voyage dans l'attribut JSON — l'état MQTT est plafonné à 255 caractères.
    // obj_id fixe l'entity_id sensor.visiophone_logs, référencé par la carte HA
    // et l'exclusion du recorder.
    {"sensor", "logs", "Logs",
     "\"obj_id\":\"visiophone_logs\",\"val_tpl\":\"{{ value_json.ts }}\","
     "\"json_attr_t\":\"visio/log\",\"ent_cat\":\"diagnostic\"",
     cfg::topic::kLog, nullptr, nullptr, nullptr, false},

    // --- Binaire --------------------------------------------------------
    {"binary_sensor", "in_call", "Appel en cours", "\"dev_cla\":\"occupancy\"",
     nullptr, nullptr, g_call, nullptr, false},
    // Caméra de la tablette. Poussé à chaque changement (publish_now) : une
    // automatisation qui allume l'écran ne peut pas attendre 30 s.
    {"binary_sensor", "presence", "Presence devant l'ecran",
     "\"dev_cla\":\"motion\"", nullptr, nullptr, g_presence, nullptr, false},
    {"button", "presence_snapshot", "Cliche camera de presence (SD)",
     "\"pl_prs\":\"PRESS\",\"ic\":\"mdi:camera\",\"ent_cat\":\"diagnostic\"",
     nullptr, "visio/presence_snapshot/set", nullptr, s_presence_snapshot,
     false},
};

// Entités RETIRÉES. Leur config de découverte a été publiée en `retain` : elle
// survit sur le broker indépendamment de nous, et Home Assistant garderait donc
// l'entité pour toujours si on se contentait de ne plus la publier. Un payload
// VIDE retenu sur le même topic est ce qui la supprime réellement.
// À conserver ici tant que des installations ont pu voir l'ancienne version.
struct RetiredEntity {
  const char *component;
  const char *object_id;
};
constexpr RetiredEntity kRetired[] = {
    // Aucune source de réveil n'est armée sur cette carte : ce bouton ne se
    // quittait qu'en coupant l'alimentation, donc en démontant l'écran mural.
    {"button", "deep_sleep"},
};

// --- Découverte, routage, publication --------------------------------------

// Résout le topic d'état d'une entité (explicite ou dérivé).
void state_topic_of(const Entity &e, char *out, size_t cap) {
  if (e.state_topic != nullptr) {
    snprintf(out, cap, "%s", e.state_topic);
  } else {
    ha_fmt::default_state_topic(out, cap, e.object_id);
  }
}

void publish_discovery() {
  char topic[128];
  char payload[512];
  for (const Entity &e : kEntities) {
    char st[96] = {0};
    const bool has_state = (e.get != nullptr) || (e.state_topic != nullptr);
    if (has_state) {
      state_topic_of(e, st, sizeof st);
    }
    const ha_fmt::EntityDesc d = {e.component,
                                  e.object_id,
                                  e.name,
                                  e.extra,
                                  has_state ? st : nullptr,
                                  e.command_topic};
    ha_fmt::discovery_topic(topic, sizeof topic, d);
    const int n = ha_fmt::discovery_payload(payload, sizeof payload, d);
    mqtt_ctrl::publish(topic, payload, n, 1, true);
  }

  // Efface du broker les configs retenues des entités retirées.
  for (const RetiredEntity &r : kRetired) {
    const ha_fmt::EntityDesc d = {r.component, r.object_id, "", "",
                                  nullptr,     nullptr};
    ha_fmt::discovery_topic(topic, sizeof topic, d);
    mqtt_ctrl::publish(topic, "", 0, 1, true);
  }
}

void subscribe_commands() {
  for (const Entity &e : kEntities) {
    if (e.set == nullptr || e.command_topic == nullptr) {
      continue;
    }
    mqtt_ctrl::subscribe(e.command_topic, 1);
  }
  // La sonnette garde son topic et son contrat de payload historiques.
  mqtt_ctrl::subscribe(cfg::topic::kRing, 1);
}

// Publie l'état d'UNE entité. `skip_sd` saute celles qui liraient la carte.
void publish_state(const Entity &e, bool skip_sd) {
  if (e.get == nullptr || (skip_sd && e.sd_touching)) {
    return;
  }
  char topic[96];
  char value[128];
  state_topic_of(e, topic, sizeof topic);
  value[0] = '\0';
  e.get(value, sizeof value);
  mqtt_ctrl::publish(topic, value, (int)strlen(value), 0, true);
}

void on_message(const char *topic, int topic_len, const char *data,
                int data_len, bool retained) {
  // Les champs ne sont pas terminés par '\0' : comparaison par longueur.
  auto matches = [&](const char *lit) {
    const size_t n = strlen(lit);
    return topic_len == (int)n && memcmp(topic, lit, n) == 0;
  };

  if (matches(cfg::topic::kRing)) {
    // Une sonnerie retenue (mosquitto_pub -r, automatisation mal réglée) se
    // rejouait à chaque reconnexion : faux appel manqué, capture SD, NVS.
    if (retained) {
      ESP_LOGW(TAG, "sonnerie retenue ignorée");
      return;
    }
    const size_t n = strlen(cfg::topic::kRingPayload);
    if (data_len == (int)n && memcmp(data, cfg::topic::kRingPayload, n) == 0 &&
        s_hooks.ring) {
      s_hooks.ring();
    }
    return;
  }

  for (const Entity &e : kEntities) {
    if (e.set == nullptr || e.command_topic == nullptr ||
        !matches(e.command_topic)) {
      continue;
    }
    // Les ACTIONS (pas de get : reboot, OTA, ouverture caméra…) ne sont pas des
    // états : retenues, elles se rejouaient à chaque reconnexion -> boucle de
    // reboot ou d'OTA. Les réglages retenus, eux, restent appliqués.
    if (retained && e.get == nullptr) {
      ESP_LOGW(TAG, "action retenue ignorée : %.*s", topic_len, topic);
      return;
    }
    e.set(data, data_len);
    // Écho immédiat : c'est ce qui rend les interrupteurs non-optimistes
    // fiables dans Home Assistant, sans attendre le tour périodique.
    publish_state(e, camera_session::active());
    return;
  }
}

void on_connected() {
  publish_discovery();
  subscribe_commands();
  const bool in_call = camera_session::active();
  for (const Entity &e : kEntities) {
    publish_state(e, in_call);
  }
}

// Republie tous les états. Pas de notification remontante depuis prefs ou
// l'UI : cela demanderait une dépendance sys/ -> app/, exactement l'inversion
// que le projet évite partout. Latence bornée à 30 s après une action tactile,
// coût assumé.
// Seuil du relevé par tâche hors session : au repos les cœurs tournent à
// 1-10 % (mesuré 2026-11-15). Le 98 % du cœur 0 observé après ~10 h de
// fonctionnement, sans appel ni rien d'animé à l'écran, n'a aucune cause
// connue : c'est lui que ce relevé doit attraper.
constexpr int kBusyCorePct = 50;

void pub_task(void *arg) {
  (void)arg;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(30000));
    // Avant le test MQTT : une saturation qui casserait le réseau doit quand
    // même laisser sa trace dans le journal SD. Hors session seulement — en
    // session le cœur 0 est saturé par construction, et log_task_cpu() de
    // video_mjpeg couvre déjà ce cas seconde par seconde.
    if (!camera_session::active()) {
      sys_stats::log_busy_tasks(kBusyCorePct);
    }
    if (!mqtt_ctrl::connected()) {
      continue;
    }
    // Pendant une session, le cœur 0 est saturé par le drain TCP du MJPEG et
    // sd_capacity() ferait une IO sur le contrôleur SDMMC partagé avec le
    // Wi-Fi : seul l'état d'appel est publié.
    const bool in_call = camera_session::active();
    for (const Entity &e : kEntities) {
      if (in_call && strcmp(e.object_id, "in_call") != 0) {
        continue;
      }
      publish_state(e, in_call);
    }
  }
}

} // namespace

void publish_now(const char *object_id) {
  if (!mqtt_ctrl::connected()) {
    return;
  }
  for (const Entity &e : kEntities) {
    if (strcmp(e.object_id, object_id) == 0) {
      publish_state(e, true);
      return;
    }
  }
}

void init(const Hooks &hooks) {
  s_hooks = hooks;
  // Contrôle de cohérence des deux copies du topic de disponibilité : ha_fmt
  // est pur et ne peut pas inclure app_config.h. Une divergence ferait publier
  // « online » sur un topic que les entités ne déclarent pas — toutes seraient
  // « indisponibles » dans Home Assistant sans qu'aucun log ne le dise.
  if (strcmp(ha_fmt::kStatusTopic, cfg::topic::kStatus) != 0) {
    ESP_LOGE(TAG, "topic de disponibilité incohérent : %s != %s",
             ha_fmt::kStatusTopic, cfg::topic::kStatus);
  }
  s_mqtt_cbs.on_message = on_message;
  s_mqtt_cbs.on_connected = on_connected;

  // Pile en PSRAM : la SRAM interne est rare (la pile WebRTC en consomme déjà
  // ~71 Ko). Priorité 2, cœur 0 : sous tout ce qui compte, ce n'est que de la
  // télémétrie.
  if (xTaskCreatePinnedToCoreWithCaps(pub_task, "ha_pub", 6144, nullptr, 2,
                                      nullptr, 0,
                                      MALLOC_CAP_SPIRAM) != pdPASS) {
    // Sans cette tâche les entités restent créées et pilotables ; seuls les
    // états cessent d'être rafraîchis. On le dit et on continue.
    ESP_LOGE(TAG, "tâche ha_pub non créée : états MQTT non rafraîchis");
  }
}

const mqtt_ctrl::Callbacks &callbacks() { return s_mqtt_cbs; }

} // namespace ha_entities
