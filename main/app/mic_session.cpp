#include "app/mic_session.h"

#include "bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app/call_history.h"
#include "media/aec.h"
#include "media/g711.h"
#include "media/ringtone.h"
#include "net/signaling.h"
#include "sys/crumb.h"
#include "sys/prefs.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace mic_session {
namespace {

constexpr const char *TAG = "mic_session";

// 160 échantillons = 20ms @ 8kHz, taille de frame RTP standard pour G.711.
constexpr size_t kFrameSamples = 160;

const char *s_url = nullptr;
signaling::Channel s_channel;

esp_peer_handle_t s_peer = nullptr;
// Flag d'arrêt lu par deux tâches sur l'autre cœur : atomic (et non volatile,
// qui ne donne aucune garantie d'ordonnancement mémoire en C++).
std::atomic<bool> s_active{false};
// Jeton unique des transitions start/stop, toutes deux déportées dans une tâche
// (mic_start / mic_stop). Interdit qu'un start rouvre le peer que mic_stop est
// en train de fermer, et inversement.
std::atomic<bool> s_transition{false};
// Sonnerie côté rue (start_ringback) : la capture est remplacée par du silence
// tant que personne n'a décroché. Lu par mic_send à chaque trame.
std::atomic<bool> s_muted{false};
// stop() tombé pendant une ouverture : elle ne peut pas être interrompue en vol
// (esp_peer_open), start_task referme donc le peer dès qu'il aboutit. Sans ça,
// un appel non répondu fermé à 30 s pendant l'ouverture laissait le backchannel
// ouvert sans écran caméra.
std::atomic<bool> s_stop_pending{false};
SemaphoreHandle_t s_peer_task_stopped = nullptr;
// mic_send_task bloque jusqu'à 1s dans i2s_channel_read, qui détient le
// sémaphore binaire du canal I2S RX pendant toute la lecture. La détruire par
// vTaskDelete laisse ce sémaphore pris à jamais : toute lecture ultérieure
// échoue avec "The channel is not enabled" (ESP_ERR_INVALID_STATE = timeout de
// prise du binaire, message trompeur du driver). D'où arrêt coopératif.
SemaphoreHandle_t s_send_task_stopped = nullptr;
SemaphoreHandle_t s_signaling_busy = nullptr;
TaskHandle_t s_send_task = nullptr;

// Ponts tâches réseau/mic -> page Micro/Debug (lus par lv_timer côté ui_mic).
// 0 Idle, 1 Connexion, 2 Connecté, 3 Échec.
std::atomic<uint8_t> s_conn{0};
std::atomic<bool> s_sdp_answer{false};
std::atomic<uint8_t> s_peak{0}; // crête PCM normalisée 0-100

// Arme l'annulation d'écho selon le réglage. Tout échec laisse le micro marcher
// sans AEC : un visiteur qui s'entend vaut mieux qu'un visiteur qu'on n'entend
// pas répondre.
void start_echo_cancel() {
  const prefs::EchoCancel mode = prefs::echo_cancel();
  if (mode == prefs::EchoCancel::kOff) {
    return;
  }
  ESP_LOGI(TAG, "décroché : AEC demandé (mode %u), SRAM interne libre %u o",
           (unsigned)mode,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  // AEC d'abord : c'est lui qui vérifie les planchers de SRAM interne. La
  // référence stéréo ensuite, parce qu'elle réalloue le DMA RX (+2,9 Ko
  // interne) — et qu'un échec à ce moment-là, l'ancien DMA déjà libéré, est
  // le seul qu'on ne veut pas risquer avec une SRAM déjà à sec.
  if (!aec::start(mode == prefs::EchoCancel::kHighPerf)) {
    return;
  }
  const esp_err_t err = bsp::Bsp::instance().set_mic_reference(true);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "référence d'écho indisponible (%s) : micro sans AEC",
             esp_err_to_name(err));
    aec::stop();
  }
}

void stop_echo_cancel() {
  if (aec::active()) {
    crumb::mark(crumb::Id::kAecStop);
    aec::stop();
    bsp::Bsp::instance().set_mic_reference(false);
  }
}

void send_task(void *arg) {
  (void)arg;
  int16_t pcm_buf[kFrameSamples];
  int16_t ref_buf[kFrameSamples];
  uint8_t alaw_buf[kFrameSamples];
  uint32_t pts = 0;
  int sent = 0;
  // Une seule tentative par session : un échec (SRAM interne) rejoué à chaque
  // trame noierait le journal et ne réussirait pas mieux.
  bool echo_tried = false;

  while (s_active.load(std::memory_order_relaxed)) {
    // Armé au décroché seulement : pendant la sonnerie côté rue la capture est
    // remplacée par du silence, il n'y a aucun écho à retirer. Et seulement
    // une fois le peer CONNECTÉ : pendant le handshake DTLS, mbedtls puise dans
    // la même SRAM interne que l'AEC et le DMA stéréo — les trois ensemble ont
    // affamé le SDIO (flux vidéo perdu puis plantage, 2026-10-25). Ici, dans la
    // tâche lectrice et entre deux lectures, comme l'exige set_mic_reference.
    if (!echo_tried && !s_muted.load(std::memory_order_relaxed) &&
        (s_conn.load() & 0x03) == 2) {
      echo_tried = true;
      start_echo_cancel();
    }
    bsp::Bsp &bsp_inst = bsp::Bsp::instance();
    esp_err_t err = aec::active()
                        ? bsp_inst.read_pcm_ref(pcm_buf, ref_buf, kFrameSamples)
                        : bsp_inst.read_pcm(pcm_buf, kFrameSamples);
    if (err != ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    // La lecture reste faite même coupée : c'est elle qui cadence l'envoi à
    // 20 ms (horloge du codec), sans elle la boucle tournerait à vide.
    if (s_muted.load(std::memory_order_relaxed)) {
      memset(pcm_buf, 0, sizeof(pcm_buf));
    } else if (aec::active()) {
      crumb::mark(crumb::Id::kAecBegin);
      aec::process(pcm_buf, ref_buf, pcm_buf, kFrameSamples); // en place
      crumb::mark(crumb::Id::kAecEnd);
    }

    // Tonalité de test (page Micro/Debug) mixée par-dessus la capture, avant
    // la crête : le VU-mètre la reflète donc sans code supplémentaire. No-op
    // tant que le bouton n'est pas armé.
    ringtone::mic_tone_mix_into(pcm_buf, kFrameSamples);

    // Crête absolue de la trame -> VU-mètre page Micro/Debug. Calcul léger
    // (160 abs), hors chemin réseau bloquant, aucun log.
    int32_t peak = 0;
    for (size_t i = 0; i < kFrameSamples; i++) {
      int32_t a =
          pcm_buf[i] < 0 ? -static_cast<int32_t>(pcm_buf[i]) : pcm_buf[i];
      if (a > peak) {
        peak = a;
      }
    }
    s_peak.store(static_cast<uint8_t>(peak * 100 / 32767));

    for (size_t i = 0; i < kFrameSamples; i++) {
      alaw_buf[i] = g711::pcm16_to_alaw(pcm_buf[i]);
    }

    esp_peer_audio_frame_t frame = {};
    frame.pts = pts;
    frame.data = alaw_buf;
    frame.size = kFrameSamples;
    if (s_peer != nullptr) {
      esp_peer_send_audio(s_peer, &frame);
    }
    pts += 20; // 20ms par frame

    sent++;
    if (sent % 100 == 1) {
      ESP_LOGD("AUDIO_DBG", "mic_send_task: sent #%d frames", sent);
    }
  }
  // Avant le give : teardown() attend ce sémaphore puis peut rouvrir une
  // session, dont la tâche reconfigurerait le RX sous nos pieds.
  stop_echo_cancel();
  xSemaphoreGive(s_send_task_stopped);
  // Pile allouée via xTaskCreateWithCaps : vTaskDelete(NULL) fuirait le bloc
  // PSRAM (tâche "statique" pour FreeRTOS, l'idle ne libère rien).
  vTaskDeleteWithCaps(nullptr);
}

void peer_loop_task(void *arg) {
  (void)arg;
  ESP_LOGD(TAG, "mic_peer_loop_task: task entered, core=%d", xPortGetCoreID());
  while (s_active.load(std::memory_order_relaxed)) {
    esp_peer_main_loop(s_peer);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  xSemaphoreGive(s_peer_task_stopped);
  vTaskDeleteWithCaps(nullptr);
}

// Pas de retry automatique côté micro (contrairement à la vidéo) :
// fonctionnalité secondaire, l'utilisateur reclique le bouton s'il veut
// réessayer.
int on_peer_msg(esp_peer_msg_t *msg, void *ctx) {
  (void)ctx;
  if (msg->type != ESP_PEER_MSG_TYPE_SDP) {
    return 0;
  }
  char *sdp = static_cast<char *>(malloc(msg->size + 1));
  if (sdp == nullptr) {
    return 0;
  }
  memcpy(sdp, msg->data, msg->size);
  sdp[msg->size] = '\0';

  if (xSemaphoreTake(s_signaling_busy, 0) != pdTRUE) {
    free(sdp);
    return 0;
  }
  ESP_LOGI(TAG, "mic: local SDP ready, signaling go2rtc");
  if (signaling::exchange(s_channel, s_peer, s_url, sdp)) {
    s_sdp_answer.store(true); // réponse SDP go2rtc injectée
  } else {
    ESP_LOGE(TAG, "mic: go2rtc signaling failed");
  }
  free(sdp);
  xSemaphoreGive(s_signaling_busy);
  return 0;
}

int on_peer_state(esp_peer_state_t state, void *ctx) {
  (void)ctx;
  ESP_LOGI(TAG, "mic: peer state -> %d", (int)state);
  if (state == ESP_PEER_STATE_CONNECTED ||
      state == ESP_PEER_STATE_DATA_CHANNEL_CONNECTED) {
    s_conn.store(2);
  } else if (state == ESP_PEER_STATE_CONNECT_FAILED ||
             state == ESP_PEER_STATE_DISCONNECTED ||
             state == ESP_PEER_STATE_CLOSED) {
    s_conn.store(3);
  } else {
    s_conn.store(1); // gathering / pairing / connecting
  }
  return 0;
}

} // namespace

bool init(const char *url) {
  s_url = url;
  // Miroir du sémaphore de signalisation vidéo : sans le give initial, le
  // premier xSemaphoreTake(..., 0) dans on_peer_msg échoue systématiquement et
  // la signalisation vers go2rtc ne part jamais.
  s_signaling_busy = xSemaphoreCreateBinary();
  s_peer_task_stopped = xSemaphoreCreateBinary();
  s_send_task_stopped = xSemaphoreCreateBinary();
  if (s_signaling_busy == nullptr || s_peer_task_stopped == nullptr ||
      s_send_task_stopped == nullptr) {
    ESP_LOGE(TAG, "création des sémaphores échouée");
    return false;
  }
  xSemaphoreGive(s_signaling_busy);
  return signaling::init_channel(s_channel, "mic");
}

bool active() { return s_active.load(std::memory_order_relaxed); }

bool live() { return active() && !s_muted.load(std::memory_order_relaxed); }

uint8_t level() { return s_peak.load(); }

uint8_t conn_state() {
  uint8_t v = s_conn.load() & 0x03;
  if (s_sdp_answer.load()) {
    v |= 0x04;
  }
  return v;
}

namespace {

void teardown(); // défini plus bas ; utilisé par le chemin d'échec de start

// Corps réel du démarrage : esp_peer_open enchaîne mbedtls_ssl_setup et les
// allocations DTLS, plusieurs centaines de ms. Jamais appelé directement depuis
// un callback LVGL (cf. start()).
void start_blocking() {
  ESP_LOGI(TAG, "start: entering (%s, interne %u o)",
           s_muted.load() ? "sonnerie rue" : "décroché",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  s_conn.store(1); // Connexion en cours
  s_sdp_answer.store(false);

  esp_peer_cfg_t peer_cfg = {};
  peer_cfg.role = ESP_PEER_ROLE_CONTROLLING;
  peer_cfg.video_dir = ESP_PEER_MEDIA_DIR_NONE;
  // SEND_RECV et pas SEND_ONLY : go2rtc inverse les directions de l'offre
  // (UnmarshalMedias) puis IsConsumer() ne renvoie true que s'il reste une
  // media qu'il doit ÉMETTRE. Une offre sendonly pure ne produit qu'une media
  // recvonly -> go2rtc classe le peer en ModePassiveProducer (publication dans
  // le flux) au lieu de ModePassiveConsumer, et seul le chemin consumer câble
  // le track micro sur le backchannel RTSP du VTO (add_consumer.go, case
  // DirectionSendonly). L'audio partait donc dans le vide malgré SRTP OK.
  // Le flux descendant que go2rtc ouvre en retour est ignoré : ce peer n'a pas
  // de on_audio_data (l'écoute passe par la connexion vidéo).
  peer_cfg.audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV;
  peer_cfg.audio_info.codec = ESP_PEER_AUDIO_CODEC_G711A;
  peer_cfg.audio_info.sample_rate = 8000;
  peer_cfg.audio_info.channel = 1;
  peer_cfg.on_state = on_peer_state;
  peer_cfg.on_msg = on_peer_msg;

  esp_err_t open_err =
      esp_peer_open(&peer_cfg, esp_peer_get_default_impl(), &s_peer);
  if (open_err != ESP_OK || s_peer == nullptr) {
    // Code brut : esp_peer renvoie des esp_peer_err_t (-1 = INVALID_ARG…),
    // pas des esp_err_t — esp_err_to_name() était trompeur. L'échec typique
    // est l'épuisement de la SRAM interne pendant mbedtls_ssl_setup (DTLS),
    // silencieux côté lib : on trace l'état du tas interne pour diagnostic.
    ESP_LOGE(TAG,
             "start: esp_peer_open -> %d (interne libre=%u, plus gros bloc=%u)",
             (int)open_err,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    s_conn.store(3); // Échec
    return;
  }

  s_active.store(true, std::memory_order_relaxed);

  // Purge des gives tardifs laissés par un arrêt précédent qui a timeout, sinon
  // le prochain stop rendrait la main avant la sortie réelle des tâches — et
  // fermerait le peer sous leurs pieds.
  xSemaphoreTake(s_peer_task_stopped, 0);
  xSemaphoreTake(s_send_task_stopped, 0);

  // Piles en PSRAM comme peer_loop : 12 Ko de piles internes aggravaient
  // l'épuisement SRAM déjà causé par la connexion vidéo.
  // Épinglée cœur 1 comme le peer_loop caméra : sans affinité, le handshake
  // DTLS (ECDHE, centaines de ms) et le POST de signalisation pouvaient tomber
  // sur le cœur 0, prioritaires (5) sur le drain MJPEG (cœur 0) — pile au
  // moment où la vidéo de l'appel démarre, puisque le ringback ouvre ce peer.
  if (xTaskCreatePinnedToCoreWithCaps(peer_loop_task, "mic_peer_loop", 8192,
                                      nullptr, 5, nullptr, 1,
                                      MALLOC_CAP_SPIRAM) != pdPASS) {
    ESP_LOGE(TAG, "start: failed to create mic_peer_loop_task");
    s_active.store(false, std::memory_order_relaxed);
    esp_peer_close(s_peer);
    s_peer = nullptr;
    s_conn.store(3);
    return;
  }

  // Remis à nullptr AVANT la création : sur échec FreeRTOS n'écrit pas la
  // sortie, et stop() attendrait alors la sortie d'un handle périmé.
  s_send_task = nullptr;
  // Épinglée cœur 1 : l'AEC y coûte jusqu'à ~40 % d'un cœur en session (mesuré
  // de nuit), et le cœur 0 est réservé au drain TCP du MJPEG, facteur limitant
  // du débit vidéo. 8 Ko et non 4 : aec_process s'exécute sur cette pile.
  if (xTaskCreatePinnedToCoreWithCaps(send_task, "mic_send", 8192, nullptr, 4,
                                      &s_send_task, 1,
                                      MALLOC_CAP_SPIRAM) != pdPASS) {
    ESP_LOGE(TAG, "start: failed to create mic_send_task");
    // Démontage direct et non stop() : on tient déjà le jeton de transition,
    // stop() le verrait pris et ne ferait rien.
    s_active.store(false, std::memory_order_relaxed);
    teardown();
    s_conn.store(3);
    return;
  }

  const esp_err_t conn_err = esp_peer_new_connection(s_peer);
  ESP_LOGI(TAG, "start: esp_peer_new_connection -> %s",
           esp_err_to_name(conn_err));
}

void teardown() {
  // Attendre la sortie réelle de mic_send_task AVANT esp_peer_close : elle
  // appelle esp_peer_send_audio(s_peer). 1500ms = pire cas read_pcm
  // (timeout I2S 1000ms) + marge.
  bool send_joined = true;
  if (s_send_task != nullptr) {
    send_joined =
        xSemaphoreTake(s_send_task_stopped, pdMS_TO_TICKS(1500)) == pdTRUE;
    if (!send_joined) {
      ESP_LOGE(TAG, "stop: mic_send_task did not exit in time");
    }
    s_send_task = nullptr;
  }
  crumb::mark(crumb::Id::kMicSendJoined, send_joined ? 1 : 0);
  // 6 s et non 1 : mic_peer_loop peut être DANS signaling::exchange(), un POST
  // HTTP bloquant borné à 4 s (timeout_ms de net/signaling.cpp) + connexion.
  // C'est le cas dès qu'on raccroche pendant l'ouverture du micro — devenu
  // courant avec la sonnerie côté rue, qui ouvre le micro à chaque appel.
  // Vu le 2026-10-25 : à 1 s le join ratait, le peer était abandonné, et la
  // session suivante démarrait avec 44 Ko de SRAM interne au lieu de 81 Ko
  // (création de tâche gcal refusée au démontage). Attendre ne fige
  // rien : teardown() tourne dans mic_stop / mic_start, jamais dans LVGL.
  const bool peer_joined =
      xSemaphoreTake(s_peer_task_stopped, pdMS_TO_TICKS(6000)) == pdTRUE;

  // Fermer le peer alors qu'une des deux tâches y touche encore le détruirait
  // sous elles (esp_peer_send_audio / esp_peer_main_loop). Sur join raté on
  // abandonne le handle : la session suivante en ouvrira un neuf, et le
  // sémaphore purgé au prochain start() absorbera le give tardif.
  if (s_peer != nullptr) {
    if (send_joined && peer_joined) {
      crumb::mark(crumb::Id::kMicPeerClose);
      esp_peer_close(s_peer);
    } else {
      ESP_LOGE(TAG, "stop: tâches non sorties, peer abandonné (fuite assumée)");
    }
    s_peer = nullptr;
  }
  crumb::mark(crumb::Id::kMicTeardownEnd);
  ESP_LOGI(TAG, "stop: stopped");
}

void teardown_task(void *arg) {
  (void)arg;
  teardown();
  s_transition.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

void start_task(void *arg) {
  (void)arg;
  start_blocking();
  // On tient encore le jeton de transition : démontage direct, comme le chemin
  // d'échec de start_blocking.
  if (s_stop_pending.exchange(false) && active()) {
    ESP_LOGI(TAG, "start: arrêt demandé pendant l'ouverture -> fermeture");
    s_conn.store(0);
    s_sdp_answer.store(false);
    s_peak.store(0);
    ringtone::mic_tone_stop();
    s_active.store(false, std::memory_order_relaxed);
    teardown();
  }
  s_transition.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

// Jeton de transition déjà pris par l'appelant.
void spawn_start() {
  s_stop_pending.store(false);
  if (xTaskCreate(start_task, "mic_start", 4096, nullptr, 4, nullptr) !=
      pdPASS) {
    ESP_LOGE(TAG, "création mic_start échouée : démarrage synchrone");
    start_blocking();
    s_transition.store(false, std::memory_order_release);
  }
}

} // namespace

// start() comme stop() sont appelés depuis des callbacks LVGL (cb_toggle_mic de
// ui_camera / ui_camera_v2, ouverture de la page Micro), donc sous le mutex
// LVGL tenu par lvgl_task EN PLEIN dispatch d'événement.
//   - start() : esp_peer_open (mbedtls + DTLS) coûte plusieurs centaines de ms.
//   - stop()  : les joins des deux tâches coûtent jusqu'à 2,5 s.
// Les jouer sur place figeait l'UI, et stop() allait jusqu'à relâcher le mutex
// de son appelant par un unlock() nu — laissant l'arbre LVGL sans verrou
// pendant toute l'attente, en concurrence avec les autres tâches qui appellent
// lv_*. Les deux transitions partent donc dans une tâche dédiée. `s_transition`
// est le jeton unique qui les sérialise : jamais un start et un stop en vol en
// même temps sur le même handle esp_peer.
void start() {
  ringtone::stop(); // le clic micro arrête la sonnerie et ouvre le talkback
  // Seul point fiable qui signale « l'utilisateur a décroché » : posé avant les
  // gardes ci-dessous, un second clic sur un micro déjà actif ne devant pas
  // défaire l'acquittement.
  call_history::on_answered();
  // Décroché pendant la sonnerie côté rue : le peer est déjà négocié, la voix
  // passe dès la trame suivante. Posé avant les gardes : si l'ouverture du
  // ringback est encore en vol, elle aboutira directement démutée.
  if (s_muted.exchange(false)) {
    ringtone::mic_tone_stop();
  }
  if (active()) {
    return;
  }
  if (s_transition.exchange(true, std::memory_order_acq_rel)) {
    ESP_LOGW(TAG, "start ignoré : transition précédente en cours");
    return;
  }
  spawn_start();
}

void start_ringback() {
  if (active()) {
    return; // re-sonnerie : la tonalité tourne déjà, ou on a décroché
  }
  if (s_transition.exchange(true, std::memory_order_acq_rel)) {
    ESP_LOGW(TAG, "ringback ignoré : transition précédente en cours");
    return;
  }
  ESP_LOGI(TAG, "ringback : backchannel coupé + ding-dong côté rue");
  // Avant l'ouverture : la première trame envoyée doit déjà être muette.
  s_muted.store(true);
  ringtone::mic_tone_start();
  spawn_start();
}

void stop() {
  if (!active()) {
    if (s_transition.load(std::memory_order_acquire)) {
      // L'ouverture en vol ne peut pas être interrompue : start_task refermera
      // le peer dès qu'il aboutit.
      ESP_LOGW(TAG, "stop pendant le démarrage : fermeture différée");
      s_stop_pending.store(true);
    } else if (s_muted.exchange(false)) {
      ringtone::mic_tone_stop(); // ringback dont l'ouverture avait échoué
    }
    return;
  }
  if (s_transition.exchange(true, std::memory_order_acq_rel)) {
    // active() vrai ET jeton pris : start_task n'a pas fini (s_active est posé
    // avant les créations de tâches et esp_peer_new_connection), ou un
    // démontage est déjà en vol. Le stop était perdu en silence dans le
    // premier cas — backchannel laissé ouvert avec le ding-dong qui tournait
    // sans fin côté rue. start_task teste ce drapeau en sortie et referme ;
    // un démontage en vol n'en a pas besoin et spawn_start le remet à zéro.
    s_stop_pending.store(true);
    s_muted.store(true);
    ringtone::mic_tone_stop();
    return;
  }
  crumb::mark(crumb::Id::kMicStop);
  s_conn.store(0); // Idle
  s_sdp_answer.store(false);
  s_peak.store(0);
  ringtone::mic_tone_stop(); // pas de tonalité orpheline sans backchannel
  // Posé AVANT la tâche : les deux boucles (send_task, peer_loop_task) doivent
  // voir le flag tomber sans attendre l'ordonnancement de mic_stop, et
  // l'appelant LVGL peut repeindre son bouton dès le retour.
  s_active.store(false, std::memory_order_relaxed);

  if (xTaskCreate(teardown_task, "mic_stop", 4096, nullptr, 4, nullptr) !=
      pdPASS) {
    // Repli : démontage synchrone, l'UI gèle le temps des joins (comportement
    // historique, dégradé mais correct — le mutex récursif reste tenu).
    ESP_LOGE(TAG, "création mic_stop échouée : démontage synchrone");
    teardown();
    s_transition.store(false, std::memory_order_release);
  }
}

} // namespace mic_session
