#include "net/signaling.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include <cstring>

namespace signaling {
namespace {

constexpr const char *TAG = "signaling";

// Une answer SDP go2rtc pèse ~1,5-3 Ko (une m-line audio, candidats ICE hôte).
// 4 Ko couvre le pire cas observé avec de la marge.
constexpr int kResponseCap = 4096;

// Accumule le corps de la réponse dans le canal passé en user_data. Le handler
// est réentrant vis-à-vis des deux connexions puisque l'état vit dans le canal.
esp_err_t http_event(esp_http_client_event_t *evt) {
  auto *ch = static_cast<Channel *>(evt->user_data);
  switch (evt->event_id) {
  case HTTP_EVENT_ON_DATA:
    if (evt->data_len + ch->response_len < ch->response_cap - 1) {
      memcpy(ch->response + ch->response_len, evt->data, evt->data_len);
      ch->response_len += evt->data_len;
    } else {
      ch->overflow = true;
    }
    break;
  case HTTP_EVENT_ON_FINISH:
    ch->response[ch->response_len] = '\0';
    break;
  default:
    break;
  }
  return ESP_OK;
}

} // namespace

bool init_channel(Channel &ch, const char *tag) {
  ch.tag = tag;
  ch.response_len = 0;
  ch.response_cap = 0;
  ch.response =
      static_cast<char *>(heap_caps_malloc(kResponseCap, MALLOC_CAP_SPIRAM));
  if (ch.response == nullptr) {
    ESP_LOGE(TAG, "%s: alloc buffer réponse (%d o) échouée", tag, kResponseCap);
    return false;
  }
  ch.response_cap = kResponseCap;
  return true;
}

bool post(Channel &ch, const char *url, const char *local_sdp) {
  if (ch.response == nullptr) {
    return false;
  }

  esp_http_client_config_t config = {};
  config.url = url;
  config.event_handler = http_event;
  config.user_data = &ch;
  config.method = HTTP_METHOD_POST;
  // 4 s et non 20 : cet appel est joué DANS peer_loop (on_peer_msg est dispatché
  // par esp_peer_main_loop), que le démontage doit pouvoir joindre. timeout_ms
  // vaut PAR opération (connexion, puis chaque lecture) : le pire cas dépasse
  // 4 s, d'où les joins de 10 s (caméra) et 6 s (micro). go2rtc répond en
  // ~200 ms sur le LAN. La reprise sur échec est assurée par
  // signaling_retry_task (5 s de cycle).
  config.timeout_ms = 4000;
  // 1 Ko : le handler accumule déjà dans ch.response (PSRAM). Ce tampon-ci est
  // sous le seuil SPIRAM_MALLOC_ALWAYSINTERNAL, donc en SRAM interne, au moment
  // même où le DTLS de l'autre peer la réclame.
  config.buffer_size = 1024;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGE(TAG, "%s: HTTP client init failed", ch.tag);
    return false;
  }

  // FORCE la fermeture de la socket après la réponse (évite l'épuisement des
  // sockets).
  esp_http_client_set_header(client, "Connection", "close");
  esp_http_client_set_header(client, "Content-Type", "text/plain");
  esp_http_client_set_post_field(client, local_sdp, strlen(local_sdp));

  ch.response_len = 0;
  ch.overflow = false;
  ch.response[0] = '\0';

  const esp_err_t err = esp_http_client_perform(client);
  bool ok = false;

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "%s: HTTP perform failed: %s", ch.tag, esp_err_to_name(err));
  } else {
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
      ESP_LOGE(TAG, "%s: HTTP bad status: %d", ch.tag, status);
    } else if (ch.response_len <= 0) {
      ESP_LOGE(TAG, "%s: empty response from go2rtc", ch.tag);
    } else if (ch.overflow) {
      // Échec franc : mieux vaut une erreur nommée qu'un peer qui ne connecte
      // jamais parce qu'il lui manque la moitié de ses candidats ICE.
      ESP_LOGE(TAG, "%s: answer SDP > %d o, tronquée -> abandon (%d o reçus)",
               ch.tag, ch.response_cap, ch.response_len);
    } else {
      ok = true;
    }
  }

  esp_http_client_cleanup(client);
  return ok;
}

void inject(Channel &ch, esp_peer_handle_t peer) {
  if (peer == nullptr || ch.response == nullptr || ch.response_len <= 0) {
    return;
  }
  esp_peer_msg_t msg = {
      .type = ESP_PEER_MSG_TYPE_SDP,
      .data = reinterpret_cast<uint8_t *>(ch.response),
      .size = static_cast<int>(ch.response_len),
  };
  esp_peer_send_msg(peer, &msg);
  ESP_LOGI(TAG, "%s: remote SDP injected successfully", ch.tag);
}

bool exchange(Channel &ch, esp_peer_handle_t peer, const char *url,
              const char *local_sdp) {
  if (peer == nullptr || !post(ch, url, local_sdp)) {
    return false;
  }
  inject(ch, peer);
  return true;
}

} // namespace signaling
