#include "app/camera_session.h"

#include "bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app/audio_pipeline.h"
#include "app/call_history.h"
#include "app/mic_session.h"
#include "app/video_mjpeg.h"
#include "media/ringtone.h"
#include "net/signaling.h"
#include "sys/sys_stats.h"
#include "ui/ui.h"
#include "ui/ui_camera_v2.h"
#include "ui/ui_weather_v3.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace camera_session {
namespace {

constexpr const char *TAG = "camera_session";

const char *s_audio_url = nullptr;
const char *s_mjpeg2_url = nullptr;

// Vue de la page caméra : la vidéo couvre tout le framebuffer, le bandeau de
// contrôles doit être re-posé après chaque décode.
const video_mjpeg::View kViewHd = {
    ui_camera_v2::video_zone_physical_rect,
    ui_camera_v2::update_stats,
    ui_camera_v2::band_blit,
    ui_camera_v2::prepare_stream,
};

// Un JPEG 800x1280 pèse ~150 Ko mesurés, allouée seulement pendant la session.
constexpr size_t kSlotHd = 320 * 1024;

signaling::Channel s_channel;

esp_peer_handle_t s_peer = nullptr;
// Flag d'arrêt lu par les tâches réseau/rendu sur l'autre cœur. atomic et non
// volatile : `volatile` en C++ n'impose que la relecture mémoire, aucune
// garantie d'ordonnancement (contrairement au `volatile` de C#).
std::atomic<bool> s_active{false};
// Un démontage est en vol dans la tâche cam_stop. Sert de garde d'unicité (un
// second appui sur Retour ne doit pas lancer une deuxième tâche) ET de verrou
// contre un start() qui rouvrirait un peer que cam_stop est en train de fermer.
std::atomic<bool> s_stopping{false};
// État du panneau AVANT la session : l'écran est poussé à 100 % le temps de
// l'appel, et doit revenir exactement d'où il venait — y compris éteint, si
// l'utilisateur l'avait coupé depuis Home Assistant. Écrit par start_common,
// lu par teardown (tâche cam_stop) : atomique.
std::atomic<bool> s_display_was_on{true};
SemaphoreHandle_t s_peer_task_stopped = nullptr;
SemaphoreHandle_t s_signaling_busy = nullptr;
// Numéro de session, incrémenté à chaque start_common(). signaling_retry_task
// capture le sien : sans ça, quitter puis rouvrir la caméra pendant son
// vTaskDelay(5000) lui faisait POSTer l'offre SDP de la session MORTE dans le
// peer de la NOUVELLE (ufrag/fingerprint dépareillés, connexion impossible) —
// pendant que l'offre légitime, elle, était jetée par la garde s_signaling_busy.
std::atomic<uint32_t> s_generation{0};
// Sonnerie côté rue demandée (request_ringback) et peer audio connecté. Le
// backchannel n'est ouvert qu'une fois CETTE connexion établie : deux handshakes
// DTLS simultanés épuisent la SRAM interne (esp_peer_open -> -1). Ordre
// séquentiellement cohérent (défaut) entre les deux drapeaux : request_ringback
// et on_peer_state posent chacun le leur puis lisent l'autre, au moins l'un des
// deux voit donc les deux et lance le ringback — exchange() garantit qu'un seul.
std::atomic<bool> s_connected{false};
std::atomic<bool> s_ringback_wanted{false};
// Une ouverture est en vol dans start_common (esp_peer_open dure ~80 ms, plus
// à la première session). Sans ce jeton, s_active ne passait à vrai qu'à la
// FIN de l'ouverture : un second trigger MQTT dans la fenêtre relançait un
// esp_peer_open concurrent (s_peer écrasé, deux peer_loop, deux mjpeg_rnd), et
// un Retour dans la fenêtre lançait un démontage pendant que start_common
// rouvrait tout derrière l'accueil — session orpheline, active() vrai à vie.
std::atomic<bool> s_starting{false};
// Retour pressé pendant l'ouverture : start_common termine puis appelle
// stop() lui-même, ce qui garde un seul chemin de démontage.
std::atomic<bool> s_stop_pending{false};
// Answer SDP obtenue par signaling_retry_task (hors peer_loop), en attente
// d'injection : vaut la génération concernée, 0 sinon. C'est peer_loop qui
// l'injecte entre deux esp_peer_main_loop — le wrapper esp_peer n'a aucun
// verrou, et esp_peer_send_msg depuis sig_retry tournait en concurrence avec
// la boucle (et sur un handle fermé si le démontage passait entre-temps).
std::atomic<uint32_t> s_answer_gen{0};

// La génération voyage dans le ctx des callbacks esp_peer : un peer abandonné
// (join raté) qui continuerait à tourner ne doit plus toucher à la session
// suivante.
uint32_t gen_of(void *ctx) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx));
}
bool is_current(void *ctx) {
  return gen_of(ctx) == s_generation.load(std::memory_order_relaxed);
}

void maybe_start_ringback() {
  if (s_connected.load() && s_ringback_wanted.exchange(false)) {
    mic_session::start_ringback();
  }
}

// Le sdp et la génération voyagent ensemble dans le void* de la tâche : un seul
// bloc à libérer, une seule convention de propriété à tenir.
struct RetryArg {
  char *sdp;
  uint32_t generation;
};

int on_audio_data(esp_peer_audio_frame_t *frame, void *ctx) {
  if (!is_current(ctx)) {
    return 0;
  }
  static int aframes = 0;
  aframes++;
  if (aframes % 50 == 1) {
    ESP_LOGD("AUDIO_DBG", "on_audio_data #%d size=%u", aframes,
             (unsigned)frame->size);
  }
  if (frame->size > 0) {
    audio_pipeline::submit_alaw(frame->data, (size_t)frame->size);
  }
  return 0;
}

void signaling_retry_task(void *arg) {
  auto *ra = static_cast<RetryArg *>(arg);
  char *sdp = ra->sdp;
  const uint32_t my_gen = ra->generation;
  int attempt = 0;
  // Borné par s_active : sans cette garde, quitter l'écran caméra pendant les
  // tentatives laissait la tâche tourner à vie (s_peer fermé => exchange()
  // échoue toujours), repeindre son message d'erreur par-dessus l'écran
  // d'accueil, et surtout garder s_signaling_busy pris — la session suivante
  // n'aurait plus jamais pu signaler.
  // Condition de survie : session toujours active ET toujours LA MÊME session.
  // s_active seul ne suffit pas — il repasse à true dès qu'une nouvelle session
  // démarre, ce qui ressuscitait cette tâche au service du mauvais peer.
  auto mine = [&] {
    return s_active.load(std::memory_order_relaxed) &&
           s_generation.load(std::memory_order_relaxed) == my_gen;
  };
  while (mine()) {
    // Attente découpée : un démontage doit voir cette tâche sortir (et rendre
    // s_signaling_busy) en 100 ms, pas au bout de 5 s — sinon une réouverture
    // rapide trouvait le canal pris et sa propre offre était jetée.
    for (int i = 0; i < 50 && mine(); i++) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!mine()) {
      break;
    }
    attempt++;
    ESP_LOGI(TAG, "Retrying go2rtc signaling (attempt %d)", attempt);
    // POST seul : l'injection revient à peer_loop (cf. s_answer_gen).
    if (signaling::post(s_channel, s_audio_url, sdp)) {
      if (mine()) {
        s_answer_gen.store(my_gen, std::memory_order_release);
      }
      ui::hide_error();
      break;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "go2rtc signaling failed\nretry #%d...", attempt);
    ui::show_error(msg);
  }
  // hide_error UNIQUEMENT si la session nous appartient encore : sinon on
  // effacerait le message d'erreur de la session suivante.
  if (!s_active.load(std::memory_order_relaxed) &&
      s_generation.load(std::memory_order_relaxed) == my_gen) {
    ui::hide_error(); // session abandonnée : pas d'erreur orpheline à l'accueil
  }
  xSemaphoreGive(s_signaling_busy);
  free(sdp);
  free(ra);
  vTaskDelete(nullptr);
}

// `sdp` ci-dessous est un buffer malloc'd dont la propriété passe d'une
// fonction/tâche à l'autre par convention (commentaires + revue), pas par le
// système de types : on_peer_msg soit le libère lui-même, soit le confie à
// xTaskCreate comme `void *arg`, et signaling_retry_task devient alors
// responsable du free() correspondant. Aucun smart pointer ici : un
// std::unique_ptr ne peut pas traverser l'argument `void*` d'une tâche
// FreeRTOS sans être déballé — l'API C des tâches précède toute notion de RAII.
int on_peer_msg(esp_peer_msg_t *msg, void *ctx) {
  if (msg->type != ESP_PEER_MSG_TYPE_SDP || !is_current(ctx)) {
    return 0;
  }
  char *sdp = static_cast<char *>(malloc(msg->size + 1));
  if (sdp == nullptr) {
    return 0;
  }
  memcpy(sdp, msg->data, msg->size);
  sdp[msg->size] = '\0';

  // Attente bornée et non immédiate : le canal peut encore être tenu par le
  // sig_retry d'une session précédente, le temps qu'il observe sa génération
  // périmée (<= 100 ms) ou finisse un POST en vol (<= ~5 s). Avec un essai à 0,
  // l'offre de la nouvelle session était jetée sans un mot : session sans son.
  // Bloquer ici ne retarde rien d'autre, la négociation n'a pas commencé.
  if (xSemaphoreTake(s_signaling_busy, pdMS_TO_TICKS(6000)) != pdTRUE) {
    ESP_LOGE(TAG, "canal de signalisation toujours occupé : offre abandonnée");
    ui::show_error("go2rtc signaling busy");
    free(sdp);
    return 0;
  }
  if (!is_current(ctx)) {
    xSemaphoreGive(s_signaling_busy);
    free(sdp);
    return 0;
  }

  ESP_LOGI(TAG, "Local SDP ready, signaling go2rtc");
  if (signaling::exchange(s_channel, s_peer, s_audio_url, sdp)) {
    free(sdp);
    xSemaphoreGive(s_signaling_busy);
    return 0;
  }

  ui::show_error("go2rtc signaling failed\nretrying...");
  auto *ra = static_cast<RetryArg *>(malloc(sizeof(RetryArg)));
  if (ra == nullptr) {
    free(sdp);
    xSemaphoreGive(s_signaling_busy);
    return 0;
  }
  ra->sdp = sdp;
  ra->generation = s_generation.load(std::memory_order_relaxed);
  if (xTaskCreate(signaling_retry_task, "sig_retry", 4096, ra, 5, nullptr) !=
      pdPASS) {
    free(ra);
    free(sdp);
    xSemaphoreGive(s_signaling_busy);
  }
  return 0;
}

int on_peer_state(esp_peer_state_t state, void *ctx) {
  if (!is_current(ctx)) {
    return 0;
  }
  ESP_LOGD(TAG, "Peer state -> %d", (int)state);
  if (state == ESP_PEER_STATE_CONNECTED) {
    // Depuis peer_loop : start_ringback ne bloque pas, il déporte l'ouverture.
    // Une ligne par session (chemin froid) : sans elle, une sonnerie côté rue
    // qui ne part pas ne laisse aucune trace de l'étape manquante.
    ESP_LOGI(TAG, "audio connecté (sonnerie rue demandée : %d, interne %u o)",
             (int)s_ringback_wanted.load(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    s_connected.store(true);
    maybe_start_ringback();
  } else if (state == ESP_PEER_STATE_DISCONNECTED ||
             state == ESP_PEER_STATE_CONNECT_FAILED ||
             state == ESP_PEER_STATE_CLOSED) {
    s_connected.store(false);
  }
  return 0;
}

// `arg` = génération de la session ; le handle est copié à l'entrée. Relire la
// globale s_peer à chaque tour faisait tourner une tâche abandonnée (join raté)
// sur le peer de la session SUIVANTE, et son give tardif tombait après la
// purge de start_common : le stop suivant fermait le peer sous la tâche vivante.
void peer_loop_task(void *arg) {
  const uint32_t my_gen = gen_of(arg);
  esp_peer_handle_t peer = s_peer;
  ESP_LOGD(TAG, "peer_loop_task: task entered, core=%d", xPortGetCoreID());
  uint32_t iter = 0;
  while (s_active.load(std::memory_order_relaxed) &&
         s_generation.load(std::memory_order_relaxed) == my_gen) {
    uint32_t want = my_gen;
    if (s_answer_gen.compare_exchange_strong(want, 0,
                                             std::memory_order_acquire)) {
      signaling::inject(s_channel, peer);
    }
    esp_peer_main_loop(peer);
    if ((iter++ % 200) == 0) {
      ESP_LOGD(TAG, "peer_loop_task: alive, iter=%lu, free_heap=%lu",
               (unsigned long)iter, (unsigned long)esp_get_free_heap_size());
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  // Give réservé à la session qui nous attend : une tâche périmée qui sort
  // pendant la session suivante validerait à tort le join de celle-ci.
  if (s_generation.load(std::memory_order_relaxed) == my_gen) {
    xSemaphoreGive(s_peer_task_stopped);
  }
  // Pile allouée via xTaskCreatePinnedToCoreWithCaps : vTaskDelete(nullptr)
  // fuit le bloc PSRAM (pile "statique" pour FreeRTOS, l'idle ne libère rien).
  // Mesuré : 16 Ko de PSRAM perdus à CHAQUE session caméra.
  vTaskDeleteWithCaps(nullptr);
}

// L'écriture console est BLOQUANTE : des dizaines de lignes par seconde
// (binding ICE, handshake DTLS, une ligne par frame audio) ralentissent les
// tâches réseau et étranglent le drain du flux MJPEG — le backlog TCP se
// traduit par plusieurs secondes de latence vidéo.
void quiet_down_libraries() {
  esp_log_level_set("BUF_MNGR", ESP_LOG_WARN);
  esp_log_level_set("AGENT", ESP_LOG_WARN);
  esp_log_level_set("DTLS", ESP_LOG_WARN);
  esp_log_level_set("PEER_DEF", ESP_LOG_WARN);
  esp_log_level_set("AUDIO_DBG", ESP_LOG_WARN);
  // Sens INVERSE des lignes ci-dessus : arme les diagnostics de performance
  // (répartition CPU par tâche + SRAM interne libre, détention du mutex LVGL),
  // deux lignes par seconde pendant les sessions caméra uniquement. Il n'y a pas
  // de console sur cette carte : c'est le seul point de bascule. Passer à false
  // supprime tout leur coût, l'échantillonnage compris.
  sys_stats::set_perf_diag(true);
}

} // namespace

bool init(const char *audio_url, const char *mjpeg_url) {
  s_audio_url = audio_url;
  s_mjpeg2_url = mjpeg_url;
  s_signaling_busy = xSemaphoreCreateBinary();
  s_peer_task_stopped = xSemaphoreCreateBinary();
  if (s_signaling_busy == nullptr || s_peer_task_stopped == nullptr) {
    ESP_LOGE(TAG, "création des sémaphores échouée");
    return false;
  }
  xSemaphoreGive(s_signaling_busy);
  return signaling::init_channel(s_channel, "cam");
}

// Ouverture en vol comprise : les gardes (trigger MQTT, OTA, net_wdt, file
// server) doivent voir la session dès son premier instant.
bool active() {
  return s_active.load(std::memory_order_relaxed) ||
         s_starting.load(std::memory_order_acquire);
}

// `static` et non un second bloc de namespace anonyme : la fonction n'est qu'un
// détail d'implémentation de start_hd(), et kViewHd/kSlotHd vivent déjà dans
// le namespace anonyme en tête de fichier.
// Sortie du plein feu : retour exactement à l'état d'avant l'appel. Éteint si
// l'utilisateur avait coupé l'écran — le rallumer parce qu'on a sonné serait
// décider à sa place.
static void restore_display() {
  if (s_display_was_on.load(std::memory_order_relaxed)) {
    bsp::Bsp::instance().restore_backlight();
  } else {
    bsp::Bsp::instance().backlight_off();
  }
}

static void start_common_locked();

static void start_common() {
  if (s_active.load(std::memory_order_relaxed) ||
      s_starting.exchange(true, std::memory_order_acq_rel)) {
    ESP_LOGW(TAG, "start ignoré : session déjà active ou en ouverture");
    return;
  }
  s_stop_pending.store(false);
  start_common_locked();
  s_starting.store(false, std::memory_order_release);
  // Retour pressé pendant l'ouverture : on démonte maintenant que tout ce qui
  // a été ouvert l'est complètement. Si l'ouverture a échoué, rien n'est
  // ouvert : on revient seulement à l'accueil.
  if (s_stop_pending.exchange(false)) {
    ESP_LOGI(TAG, "arrêt demandé pendant l'ouverture -> démontage");
    if (s_active.load(std::memory_order_relaxed)) {
      stop();
    } else {
      bsp::Bsp::LvglLock lvgl_lock;
      ui::load_home_screen();
    }
  }
}

static void start_common_locked() {
  // cam_stop peut encore être en train de fermer s_peer : rouvrir maintenant
  // écraserait le handle qu'il s'apprête à passer à esp_peer_close(). On ATTEND
  // plutôt qu'on refuse — un coup de sonnette arrivant juste après un retour
  // manuel doit ouvrir la vidéo, pas tomber dans le vide. Toujours appelé
  // depuis cam_defer / cam2_defer, jamais depuis la tâche LVGL : dormir ici ne
  // fige rien. Borne alignée sur le pire cas du démontage (join réseau 7 s).
  // 22 s : pire cas de teardown() = joins vidéo (7 + 3 s) + peer (10 s).
  for (int i = 0; i < 220 && s_stopping.load(std::memory_order_acquire);
       i++) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (s_stopping.load(std::memory_order_acquire)) {
    ESP_LOGE(TAG, "start abandonné : démontage précédent toujours en cours");
    s_ringback_wanted.store(false);
    ui::show_error("Session précédente bloquée\nRéessayez");
    return;
  }
  // Nouvelle génération AVANT toute création de tâche : un sig_retry de la
  // session précédente encore endormi verra son numéro périmé au réveil.
  const uint32_t gen =
      s_generation.fetch_add(1, std::memory_order_relaxed) + 1;
  s_answer_gen.store(0);
  // s_ringback_wanted, lui, n'est PAS remis à zéro : le trigger MQTT le pose
  // avant que cam2_defer n'arrive ici.
  s_connected.store(false);

  // Plein feu pendant l'appel : l'écran peut être en veille ou à 20 % la nuit,
  // et on veut voir le visiteur sans toucher la dalle. Voie TEMPORAIRE : passer
  // par set_backlight() persisterait 100 % en NVS et écraserait le réglage de
  // l'utilisateur — définitivement si le courant tombait pendant l'appel.
  s_display_was_on.store(bsp::Bsp::instance().display_on(),
                         std::memory_order_relaxed);
  bsp::Bsp::instance().set_backlight_temporary(100);

  quiet_down_libraries();
  ESP_LOGI(TAG, "start: entering");

  // Empreinte mémoire avant/après ouverture WebRTC : la pile DTLS n'accepte que
  // de la SRAM interne, dont l'épuisement est le mode d'échec classique.
  ESP_LOGI(TAG, "Mémoire AVANT WebRTC - Interne: %lu octets, PSRAM: %lu octets",
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  esp_peer_cfg_t peer_cfg = {};
  peer_cfg.role = ESP_PEER_ROLE_CONTROLLING;
  // Vidéo servie par MJPEG : WebRTC audio-only. NONE = pas de m-line vidéo dans
  // l'offre SDP -> go2rtc n'envoie plus le H.264 (économie WiFi, évite le
  // backlog TCP du flux MJPEG concurrent).
  peer_cfg.video_dir = ESP_PEER_MEDIA_DIR_NONE;
  peer_cfg.audio_dir = ESP_PEER_MEDIA_DIR_RECV_ONLY;
  peer_cfg.audio_info.codec = ESP_PEER_AUDIO_CODEC_G711A;
  peer_cfg.audio_info.sample_rate = 8000;
  peer_cfg.audio_info.channel = 1;
  peer_cfg.on_state = on_peer_state;
  peer_cfg.on_msg = on_peer_msg;
  peer_cfg.on_audio_data = on_audio_data;
  peer_cfg.ctx = reinterpret_cast<void *>(static_cast<uintptr_t>(gen));

  esp_err_t open_err =
      esp_peer_open(&peer_cfg, esp_peer_get_default_impl(), &s_peer);
  if (open_err != ESP_OK || s_peer == nullptr) {
    // esp_peer renvoie des esp_peer_err_t (-1 = INVALID_ARG…), pas des
    // esp_err_t : on logge le code brut. L'échec typique est l'épuisement de la
    // SRAM interne dans mbedtls_ssl_setup (DTLS).
    ESP_LOGE(TAG, "start: esp_peer_open -> %d (interne libre=%u)",
             (int)open_err,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ui::show_error("Erreur ouverture WebRTC");
    restore_display(); // teardown() ne sera pas appelé sur ce chemin
    s_ringback_wanted.store(false); // stop() non plus : la demande mourrait armée
    return;
  }

  // Les deux framebuffers DSI peuvent contenir du rendu périmé : re-tamponner
  // tous les widgets suivis dans les deux et nettoyer la colonne de contrôles
  // pour que le double-buffer ait un contenu cohérent avant la première frame
  // vidéo.
  ui_camera_v2::prepare_stream();
  s_active.store(true, std::memory_order_relaxed);

  // Purge d'un give tardif laissé par un arrêt précédent qui a timeout. Sans
  // elle le prochain stop() rendrait la main IMMÉDIATEMENT, croyant peer_loop
  // sortie, et appellerait esp_peer_close() pendant qu'elle est dans
  // esp_peer_main_loop(). Même précaution que mic_session pour mic_send.
  xSemaphoreTake(s_peer_task_stopped, 0);

  // Pile en PSRAM (xTaskCreateWithCaps) et non en SRAM interne : cette boucle
  // porte le déchiffrement SRTP et les callbacks audio, et la SRAM interne est
  // déjà tendue par la pile WebRTC.
  // Épinglée core 1 : core 0 est réservé au drain TCP du flux MJPEG (cf.
  // affinité dans mjpeg_client::start), qui est le facteur limitant du débit.
  if (xTaskCreatePinnedToCoreWithCaps(peer_loop_task, "peer_loop", 16384,
                                      reinterpret_cast<void *>(
                                          static_cast<uintptr_t>(gen)),
                                      5, nullptr, 1,
                                      MALLOC_CAP_SPIRAM) != pdPASS) {
    ESP_LOGE(TAG, "Échec de la création de peer_loop_task en PSRAM !");
    ui::show_error("Erreur allocation mémoire tâche");
    s_active.store(false, std::memory_order_relaxed);
    esp_peer_close(s_peer);
    s_peer = nullptr;
    restore_display(); // teardown() ne sera pas appelé sur ce chemin
    s_ringback_wanted.store(false); // stop() non plus : la demande mourrait armée
    return;
  }

  esp_err_t conn_err = esp_peer_new_connection(s_peer);
  ESP_LOGI(TAG, "start: esp_peer_new_connection -> %s",
           esp_err_to_name(conn_err));

  ESP_LOGI(TAG, "Mémoire APRÈS WebRTC - Interne: %lu octets, PSRAM: %lu octets",
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  // Vidéo MJPEG (décodage HW) : démarrée après prepare_stream (zones nettoyées)
  // et s_active=true. Le rendu se fait dans la tâche du pipeline MJPEG.
  if (!video_mjpeg::start(s_mjpeg2_url, kSlotHd, kViewHd)) {
    // Sans tâche de rendu, la bannière « flux perdu » ne s'affiche jamais :
    // l'écran restait noir sans explication.
    ESP_LOGE(TAG, "start: démarrage vidéo MJPEG échoué");
    ui::show_error("Erreur démarrage vidéo");
  }
}

void start_hd() { start_common(); }

void request_ringback() {
  ESP_LOGI(TAG, "sonnerie rue demandée (audio connecté : %d)",
           (int)s_connected.load());
  s_ringback_wanted.store(true);
  maybe_start_ringback();
}

namespace {

// Aucun destructeur ne joue cette séquence : l'ordre de démontage (arrêt des
// tâches -> fermeture du peer -> retour à l'écran d'accueil) est entièrement
// écrit à la main et doit être revérifié à chaque modification de cette
// fonction. L'attente explicite du sémaphore garantit que la tâche qui touche
// ces handles est réellement sortie avant qu'ils soient détruits.
void teardown() {
  // AVANT video_mjpeg::stop() : celui-ci rend les slots de la mailbox au tas, et
  // c'est de là que sort la capture archivée pour un appel manqué. Écriture SD
  // bloquante — acceptable ici, cette fonction tourne dans la tâche cam_stop
  // (sauf sur le repli synchrone déjà documenté dans stop(), où l'UI gèle de
  // toute façon).
  const bool missed_nav = call_history::on_camera_closing();

  // s_active=false (posé par stop()) fait déjà retourner les callbacks sans
  // toucher au framebuffer ; on arrête réseau + tâche de rendu.
  video_mjpeg::stop();

  // 10 s et non 1 : peer_loop peut être DANS on_peer_msg (attente du canal
  // <= 6 s puis POST <= ~5 s, timeout par opération). À 1 s le join ratait et
  // le peer entier (SRAM DTLS) était abandonné — vu au boot sous IDF 6.0.1 ; le
  // micro avait déjà été porté à 6 s pour la même raison.
  const bool peer_joined =
      xSemaphoreTake(s_peer_task_stopped, pdMS_TO_TICKS(10000)) == pdTRUE;

  // Fermer le peer alors que peer_loop est encore DANS esp_peer_main_loop() le
  // détruit sous elle. Le cas se produit quand on_peer_msg est en train de jouer
  // signaling::exchange() (POST HTTP bloquant) : la tâche ne peut pas observer
  // s_active avant que l'appel rende la main. Sur join raté on abandonne le
  // handle — mêmes sémantiques que mic_session::teardown, dont cette fonction
  // avait divergé. Le sémaphore purgé au prochain start() absorbe le give tardif.
  if (s_peer != nullptr) {
    if (peer_joined) {
      esp_peer_close(s_peer);
    } else {
      ESP_LOGE(TAG, "peer_loop non sortie : peer abandonné (fuite assumée)");
    }
    s_peer = nullptr;
  }

  restore_display(); // fin du plein feu

  bsp::Bsp::LvglLock lvgl_lock;
  bsp::Bsp::instance().set_direct_mode(false);
  // Personne n'a répondu : on atterrit sur la page météo, où la pastille
  // d'appels manqués attend. Un appel répondu — ou un accès manuel à la caméra —
  // revient à l'accueil comme avant.
  if (missed_nav) {
    ui_weather_v3::open_screen();
  } else {
    ui::load_home_screen();
  }

  // Marge de pile restante, une ligne par fin d'appel (chemin froid). C'est la
  // séquence la plus profonde de l'application — FATFS + stdio + NVS dans
  // on_camera_closing, puis le chargement de l'écran le plus lourd — et le
  // canari FreeRTOS ne se relit qu'au changement de contexte : sans cette
  // mesure, un dépassement se manifeste par un reboot à un endroit sans rapport
  // avec le vrai coupable. Si cette marge redescend sous ~1 Ko, agrandir la
  // pile de cam_stop plutôt que d'ajouter du travail ici.
  ESP_LOGI(TAG, "cam_stop : marge de pile %u o",
           (unsigned)(uxTaskGetStackHighWaterMark(nullptr) *
                      sizeof(StackType_t)));
}

void teardown_task(void *arg) {
  (void)arg;
  teardown();
  s_stopping.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

} // namespace

// Appelé depuis cb_back(), donc sous le mutex LVGL tenu par lvgl_task, elle-même
// EN COURS de dispatch d'événement. Le démontage dure de 1 à 7 s (join du read
// réseau MJPEG) : le jouer ici obligeait à relâcher le mutex de l'appelant par
// un unlock() nu, ce qui laissait lvgl_task manipuler l'arbre LVGL sans verrou
// pendant toute l'attente — en concurrence avec ui::show_error() depuis
// mjpeg_rnd et on_fetch_done() depuis weather_fetch. On rend donc la main tout
// de suite et le démontage part dans sa propre tâche, qui reprend le LvglLock
// proprement pour le retour à l'accueil.
void stop() {
  // Ouverture en vol : start_common rappellera stop() une fois tout ouvert.
  // Drapeau posé AVANT le test, et start_common le relit APRÈS avoir rendu son
  // jeton : l'un des deux voit forcément l'autre (ordre séquentiel par défaut).
  s_stop_pending.store(true);
  if (s_starting.load()) {
    ringtone::stop();
    return;
  }
  s_stop_pending.store(false);
  if (s_stopping.exchange(true, std::memory_order_acq_rel)) {
    return; // démontage déjà en vol (double appui sur Retour)
  }
  s_active.store(false, std::memory_order_relaxed);
  s_connected.store(false);
  s_ringback_wanted.store(false);
  ringtone::stop(); // ne jamais laisser la sonnerie armée hors appel
  // Toutes les fermetures passent ici (Retour, non-réponse à 30 s) : le
  // backchannel ouvert par la sonnerie côté rue ne doit pas survivre à l'écran.
  mic_session::stop();
  // Hors sonnerie, aud_render dort en portMAX_DELAY sur sa file de slots. Plus
  // aucun submit_alaw n'arrive après ce point : sans réveil explicite la tâche
  // ne repasse jamais en haut de boucle, ne voit pas la transition
  // streaming->arrêt et ne pousse pas ses frames de silence — le DMA I2S
  // reboucle alors le dernier chunk distant (son résiduel au retour à l'accueil).
  audio_pipeline::wake();

  // 10 Ko et non les 4 Ko d'origine : teardown() ne se contente plus de fermer
  // des handles et de charger un écran. Depuis l'historique d'appels il ouvre la
  // séquence call_history::on_camera_closing() -> FATFS + stdio (fwrite d'un
  // JPEG de ~150 Ko, fprintf du journal) -> NVS (nvs_set_str + nvs_commit), et
  // seulement ensuite lv_scr_load de l'écran météo. À 4 Ko la pile débordait :
  // canari FreeRTOS -> panic -> reboot instantané (PANIC_REBOOT_DELAY=0), APRÈS
  // que la capture soit écrite sur la carte — d'où un appel manqué correctement
  // archivé mais jamais affiché. Le log de marge en fin de teardown() surveille
  // ce budget. SRAM interne : ~24 Ko libres en session, cette pile en prend 10.
  // Priorité 4 = celle des tâches cam_defer symétriques.
  if (xTaskCreate(teardown_task, "cam_stop", 10240, nullptr, 4, nullptr) !=
      pdPASS) {
    // Repli : démontage synchrone. Le mutex LVGL reste tenu (récursif, la prise
    // de teardown() est donc gratuite) et l'UI gèle le temps de l'arrêt — c'est
    // le comportement historique, dégradé mais pas incorrect.
    ESP_LOGE(TAG, "création cam_stop échouée : démontage synchrone");
    teardown();
    s_stopping.store(false, std::memory_order_release);
  }
}

} // namespace camera_session
