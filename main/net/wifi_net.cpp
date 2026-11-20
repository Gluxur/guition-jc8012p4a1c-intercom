#include "wifi_net.h"

#include "app_config.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <atomic>
#include <string.h>
#include <time.h>

namespace wifi_net {

static const char *TAG = "wifi_net";


static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

// L'event group ne sert qu'à débloquer connect_blocking une fois : son bit
// n'est jamais remis à zéro sur déconnexion. Ce flag-ci suit l'état courant.
static std::atomic<bool> s_link_up{false};

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    s_link_up.store(false);
    // La raison est le champ le plus utile pour diagnostiquer un boîtier qui
    // n'associe pas (mot de passe, AP saturé, hors de portée) : elle était
    // jetée. Une ligne par déconnexion, jamais dans un chemin chaud.
    const auto *d = static_cast<wifi_event_sta_disconnected_t *>(event_data);
    ESP_LOGW(TAG, "déconnexion, raison=%d", d != nullptr ? (int)d->reason : -1);
    esp_wifi_connect();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    s_link_up.store(true);
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

bool connect_blocking(const char *ssid, const char *pass) {
  s_wifi_event_group = xEventGroupCreate();
  if (s_wifi_event_group == nullptr) {
    // Le handler d'événement fait xEventGroupSetBits() dessus : avec les
    // assertions désactivées, un handle nul y devient un déréférencement NULL
    // depuis la tâche de la boucle d'événements.
    ESP_LOGE(TAG, "xEventGroupCreate a échoué");
    return false;
  }
  // Assertions désactivées (sdkconfig perf) : ESP_ERROR_CHECK masquerait les
  // erreurs, on vérifie manuellement.
  esp_err_t err = esp_netif_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
    return false;
  }
  err = esp_event_loop_create_default();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(err));
    return false;
  }
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
    return false;
  }

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  // Sans handler enregistré, s_link_up reste faux à vie et l'event group n'est
  // jamais armé : la connexion réussirait sans que personne ne l'apprenne.
  esp_err_t h1 = esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
  esp_err_t h2 = esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);
  if (h1 != ESP_OK || h2 != ESP_OK) {
    ESP_LOGE(TAG, "enregistrement des handlers: %s / %s", esp_err_to_name(h1),
             esp_err_to_name(h2));
    return false;
  }

  // `= {}` is C++ value-initialization: zeroes every byte of this (nested,
  // union-containing) C struct, including padding and fields not touched
  // below (channel, bssid, etc.) — equivalent to `default(WifiConfig)` in
  // C#. C'est aussi ce qui garantit la terminaison des copies ci-dessous.
  wifi_config_t wifi_config = {};
  // Copie bornée (et non strcpy) : sta.ssid fait 32 octets, sta.password 64,
  // alors que settings borne les champs à 32 et 64 CARACTÈRES + NUL. Un SSID de
  // 32 caractères faisait donc écrire 33 octets, débordant sur le champ voisin
  // de la structure. Le driver lit ces champs en strnlen : une chaîne qui
  // remplit exactement le tableau, sans NUL, reste valide.
  memcpy(wifi_config.sta.ssid, ssid,
         strnlen(ssid, sizeof(wifi_config.sta.ssid)));
  memcpy(wifi_config.sta.password, pass,
         strnlen(pass, sizeof(wifi_config.sta.password)));

  esp_err_t m_err = esp_wifi_set_mode(WIFI_MODE_STA);
  esp_err_t c_err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  if (m_err != ESP_OK || c_err != ESP_OK) {
    ESP_LOGE(TAG, "set_mode/set_config: %s / %s", esp_err_to_name(m_err),
             esp_err_to_name(c_err));
    return false;
  }
  err = esp_wifi_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
    return false;
  }

  // Power-save WiFi (activé par défaut sur esp_hosted) fait arriver les
  // paquets RTP par rafales espacées de 200-1200ms : les access units IDR
  // multi-paquets (STAP-A + ~7 FU-A) perdent presque toujours un fragment et
  // sont droppées entières par esp_peer -> image pixelisée, premier décodage
  // tardif. PS_NONE garantit une réception continue.
  esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
  ESP_LOGI(TAG, "esp_wifi_set_ps(WIFI_PS_NONE) -> %s", esp_err_to_name(ps_err));

  // Horloge murale AVANT l'attente d'IP : le P4 n'a pas de RTC. Lancé après
  // l'obtention de l'IP, un boot sans réseau à 60 s (box plus lente que la
  // tablette après une coupure de courant) ne démarrait JAMAIS SNTP ni le
  // fuseau : 1970 et UTC jusqu'au reboot suivant (météo "--", journaux datés
  // 0000-00-00). Le client SNTP lwIP retente seul jusqu'à ce que l'interface
  // monte. L'UI lit localtime() et affiche "--" tant que la synchro manque.
  setenv("TZ", cfg::kTimezone, 1);
  tzset();
  esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(cfg::kSntpServer);
  esp_err_t sntp_err = esp_netif_sntp_init(&sntp_cfg);
  if (sntp_err != ESP_OK) {
    ESP_LOGE(TAG, "esp_netif_sntp_init: %s", esp_err_to_name(sntp_err));
  }

  // 60 s et non portMAX_DELAY : au-delà, on rend la main pour que app_main
  // démarre quand même MQTT, OTA et net_wdt. Le driver continue à retenter
  // (handler STA_DISCONNECTED) : si le lien monte plus tard, tout se raccroche
  // tout seul. Auparavant un SSID erroné figeait le boot ici définitivement.
  const EventBits_t bits =
      xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE,
                          pdTRUE, pdMS_TO_TICKS(60000));
  if ((bits & WIFI_CONNECTED_BIT) == 0) {
    ESP_LOGE(TAG, "pas d'IP après 60 s : boot poursuivi sans réseau");
    return false;
  }
  ESP_LOGI(TAG, "WiFi connected");
  return true;
}

int wifi_rssi(void) {
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
    return 0;
  }
  return ap.rssi;
}

bool link_up(void) { return s_link_up.load(); }

} // namespace wifi_net
