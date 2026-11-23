#include "app/video_mjpeg.h"

#include "bsp.h"
#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "net/mjpeg_client.h"
#include "net/wifi_net.h"
#include "sys/mutex.h"
#include "sys/i18n.h"
#include "sys/prefs.h"
#include "sys/sys_stats.h"
#include "ui/ui.h"

#include <atomic>
#include <cstdio>
#include <cstdlib> // free() : la sortie du décodeur vient de jpeg_alloc_decoder_mem
#include <cstring> // memcpy : archivage d'une frame de la mailbox

using i18n::Str;

namespace video_mjpeg {
namespace {

constexpr const char *TAG = "MJPEG";
// Tag distinct du flux MJPEG : permet d'armer la mesure du mutex LVGL seule,
// sans passer tout le pipeline vidéo en DEBUG.
constexpr const char *kLockTag = "LVGL_MUX";

// Capacité d'un slot de mailbox, fixée par l'appelant au start() : elle dépend
// de la résolution du flux (un JPEG 800x640 à la qualité go2rtc par défaut pèse
// ~37 Ko mesurés, un 800x1280 monte à ~150 Ko). Une frame plus grosse est
// abandonnée et resynchronisée sur le SOI suivant.
size_t s_frame_cap = 0;
// Capacité réellement allouée : réallouer les slots si la session suivante
// demande une autre taille.
size_t s_mb_cap = 0;
// Un join réseau raté laisse la tâche mjpeg écrire dans un slot emprunté. Les
// slots sont alors définitivement perdus : ni free_mailbox() ni la réallocation
// d'une autre taille (128 Ko <-> 320 Ko au changement de page caméra) ne doivent
// plus y toucher, sinon on rend au tas de la PSRAM sous un memcpy en cours.
bool s_mb_abandoned = false;
View s_view = {};

jpeg_decoder_handle_t s_dec = nullptr;
uint8_t *s_out = nullptr; // sortie RGB565 du décodeur
size_t s_out_cap = 0;
StreamingProbe s_is_streaming = nullptr;
bool s_path_logged = false; // chemin rendu déjà annoncé pour cette session ?

// Mailbox JPEG double-buffer "last frame wins" entre la tâche réseau (produit)
// et la tâche de rendu (consomme). Découple le réseau du décodage+rendu : si le
// rendu (~30-40ms/frame) ne suit pas les 25fps réseau, on ÉCRASE la frame non
// consommée au lieu d'accumuler du retard dans le buffer TCP. Sans ça, la vidéo
// prend un retard croissant -> désync audio (son en avance).
uint8_t *s_mb[2] = {nullptr, nullptr};
size_t s_mb_len[2] = {0, 0};
int s_ready = -1;     // index publié, prêt à décoder ; -1 sinon
int s_rendering = -1; // index en cours de rendu
int s_writing = -1; // index prêté à la tâche réseau (accumulation en cours)
// Dernier index PUBLIÉ, jamais remis à -1 par la consommation. s_ready et
// s_rendering retombent tous deux à -1 dès que la tâche de rendu a fini sa
// frame : à la fin d'un appel — le seul moment où l'on archive une capture —
// ils ne désignent donc plus rien alors que le slot, lui, contient toujours la
// dernière image reçue. C'est ce slot que capture_snapshot va chercher.
int s_last = -1;
SemaphoreHandle_t s_mutex = nullptr;
SemaphoreHandle_t s_sem = nullptr;
std::atomic<bool> s_render_run{false};
SemaphoreHandle_t s_render_stopped = nullptr;

// Compteurs de diagnostic (écrits sous s_mutex par la tâche réseau, lus + remis
// à 0 par la tâche de rendu dans le log 1x/s). recv = frames reçues du réseau ;
// drop = frames écrasées dans la mailbox avant rendu (= rendu trop lent =>
// borné par le calcul). recv ~= fps affiché => borné par le réseau/bande
// passante. bytes = octets JPEG reçus (débit).
uint32_t s_recv = 0;
uint32_t s_drop = 0;
uint64_t s_bytes = 0;

// --- Détection de perte de flux ---------------------------------------------
// Sur coupure réseau, mjpeg_client reconnecte en silence (open en échec toutes
// les ~6 s) : l'image reste FIGÉE à l'écran, sans rien pour distinguer une
// panne d'une scène immobile. Ces deux champs n'appartiennent qu'à la tâche de
// rendu — aucun verrou.
// 5 s : le premier décode arrive ~3 s après start() (ouverture HTTP + première
// frame complète). Un seuil plus court afficherait le bandeau à chaque
// démarrage de session.
constexpr int64_t kStreamLostUs = 5000000;
// Avant la PREMIÈRE frame : un démarrage à froid de go2rtc (ffmpeg + RTSP +
// attente d'une IDR) prend ~2,6-3,3 s mesurés, davantage si l'hôte est chargé.
// À 5 s le bandeau « flux perdu » tombait sur un simple démarrage lent.
constexpr int64_t kFirstFrameGraceUs = 12000000;
int64_t s_last_frame_us = 0;
bool s_lost = false;
// ui::error_displayed() est GLOBAL : camera_session le pose aussi pour les
// échecs de signalisation AUDIO. Suspendre le rendu là-dessus figeait l'image
// alors que le flux MJPEG arrivait à 25 fps — et rien ne pouvait plus le
// débloquer, banner_clear() rafraîchissant s_last_frame_us à chaque frame
// consommée. Seul NOTRE bandeau doit suspendre le rendu.
bool video_banner_up() { return s_lost; }

// Accumulateurs du log perf 1x/s + compteur d'erreurs de décodage. N'appartient
// qu'à la tâche de rendu ; remis à zéro par start() pour que chaque session
// reparte d'une fenêtre propre.
struct Perf {
  uint32_t frames = 0;
  int64_t t_flip = 0, t_dec = 0, t_ppa = 0, t_band = 0, t_pres = 0;
  int64_t tw = 0;
  uint32_t decode_errs = 0;
};
Perf s_perf;

bool streaming() { return s_is_streaming != nullptr && s_is_streaming(); }

// Bandeau « flux perdu ». show_error coupe le mode direct : LVGL reprend la
// main et repeint l'écran caméra, le bandeau devient donc visible alors même
// que plus aucune frame n'arrive.
void banner_lost() {
  if (s_lost || !streaming()) {
    return;
  }
  // s_path_logged = première frame décodée de la session.
  const int64_t limit = s_path_logged ? kStreamLostUs : kFirstFrameGraceUs;
  if (esp_timer_get_time() - s_last_frame_us < limit) {
    return;
  }
  s_lost = true;
  ESP_LOGW(TAG, "flux perdu : aucune frame depuis %d s", (int)(kStreamLostUs / 1000000));
  ui::show_error(i18n::t(Str::kCamStreamLost));
}

// Appelée à chaque frame consommée, AVANT decode_render : celui-ci refuse
// d'écrire tant que ui::error_displayed() est vrai (il écraserait le message).
void banner_clear() {
  s_last_frame_us = esp_timer_get_time();
  if (!s_lost) {
    return;
  }
  s_lost = false;
  ESP_LOGI(TAG, "flux rétabli");
  ui::hide_error();
  // Les deux framebuffers DSI ont divergé pendant l'affichage du bandeau (LVGL
  // en mode PARTIAL n'a repeint que celui en scan-out) : la page les réaligne.
  if (s_view.restore != nullptr) {
    s_view.restore();
  }
}

// --- Sink du client réseau : prête un slot de mailbox, zéro copie ------------

uint8_t *sink_begin(size_t *cap, void *) {
  if (!streaming()) {
    return nullptr;
  }
  sys::MutexLock lock(s_mutex);
  // On préfère un slot totalement libre ; si les deux sont pris (un en rendu,
  // l'autre publié non consommé), on sacrifie le publié : c'est exactement la
  // sémantique "last frame wins" de cette mailbox.
  int idx;
  if (s_rendering != 0 && s_ready != 0) {
    idx = 0;
  } else if (s_rendering != 1 && s_ready != 1) {
    idx = 1;
  } else {
    idx = (s_rendering == 0) ? 1 : 0;
    s_drop++;
    s_ready = -1;
  }
  if (s_mb[idx] == nullptr) {
    return nullptr;
  }
  s_writing = idx;
  *cap = s_frame_cap;
  return s_mb[idx];
}

void sink_end(size_t len, void *) {
  sys::MutexLock lock(s_mutex);
  const int idx = s_writing;
  s_writing = -1;
  if (idx < 0) {
    return;
  }
  if (s_ready >= 0) {
    s_drop++; // frame publiée jamais consommée (rendu en retard)
  }
  s_mb_len[idx] = len;
  s_ready = idx;
  s_last = idx;
  s_recv++;
  s_bytes += len;
  xSemaphoreGive(s_sem);
}

void sink_abort(void *) {
  sys::MutexLock lock(s_mutex);
  s_writing = -1;
}

// --- Décodage matériel + rendu PPA -----------------------------------------

// Décode une frame JPEG (HW) et l'affiche via PPA -> framebuffer DSI.
void decode_render(const uint8_t *jpeg, size_t len) {
  if (!streaming() || video_banner_up()) {
    return;
  }

  jpeg_decode_picture_info_t info = {};
  if (jpeg_decoder_get_info(jpeg, len, &info) != ESP_OK) {
    return;
  }

  if (s_view.rect == nullptr) {
    return;
  }
  uint32_t vzx, vzy, vzw, vzh;
  s_view.rect(vzx, vzy, vzw, vzh);

  // Le back-buffer n'est réellement libre qu'une fois la bascule armée par le
  // present_frame() précédent prise par le panneau (fin de la trame DSI en
  // cours, jusqu'à ~31 ms). Écrire avant, c'est repeindre le buffer AFFICHÉ :
  // en page V2 le décodeur couvre tout le framebuffer, le bourrage noir du
  // flux réapparaissait donc par blocs sous le bandeau de contrôles, jusqu'à
  // ce que band_blit() les recouvre. Attente nulle en pratique — l'intervalle
  // entre deux frames MJPEG (~40 ms) dépasse déjà la période panneau.
  int64_t tf0 = esp_timer_get_time();
  bsp::Bsp::instance().wait_frame_flip();
  int64_t tf1 = esp_timer_get_time();

  // Chemin zéro copie : si la source sort déjà à la largeur du panneau, le
  // décodeur écrit directement dans le framebuffer DSI et le PPA (22 ms pour
  // une copie 1:1 de 800x640) disparaît du chemin chaud. nullptr = géométrie
  // incompatible -> chemin PPA historique.
  size_t direct_cap = 0;
  uint8_t *direct = bsp::Bsp::instance().direct_video_target(
      info.width, info.height, vzx, vzy, vzw, vzh, &direct_cap);

  // Le décodeur JPEG HW aligne la sortie sur 16 px (w et h) : dimensionner le
  // buffer sur les dimensions alignées, sinon ESP_ERR_INVALID_ARG.
  uint32_t aw = (info.width + 15) & ~15u;
  uint32_t ah = (info.height + 15) & ~15u;
  size_t need = (size_t)aw * ah * 2; // RGB565
  if (direct == nullptr && s_out_cap < need) {
    if (s_out != nullptr) {
      free(s_out);
      s_out = nullptr;
      s_out_cap = 0;
    }
    jpeg_decode_memory_alloc_cfg_t oc = {JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    size_t got = 0;
    s_out = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(need, &oc, &got));
    if (s_out == nullptr) {
      ESP_LOGE(TAG, "alloc sortie %u o échouée", (unsigned)need);
      return;
    }
    s_out_cap = got;
  }

  jpeg_decode_cfg_t dcfg = {};
  dcfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  // BGR = "small endian" dans la convention du driver JPEG ESP = RGB565
  // little-endian, l'ordre attendu par le PPA/panneau. RGB (="big endian")
  // sortait du RGB565 byte-swappé -> couleurs éclatées (arc-en-ciel).
  dcfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
  dcfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
  uint32_t out_size = 0;
  int64_t t0 = esp_timer_get_time();
  esp_err_t err =
      jpeg_decoder_process(s_dec, &dcfg, jpeg, len, direct ? direct : s_out,
                           direct ? direct_cap : s_out_cap, &out_size);
  if (err != ESP_OK) {
    if ((++s_perf.decode_errs % 100) == 1) {
      ESP_LOGW(TAG, "decode err %s (%lux%lu)", esp_err_to_name(err),
               (unsigned long)info.width, (unsigned long)info.height);
    }
    return;
  }
  int64_t t1 = esp_timer_get_time();

  // En chemin direct l'image est DÉJÀ dans le framebuffer : rien à recopier.
  // Sinon, rendu PPA RGB565 -> zone vidéo. Le décodeur JPEG HW écrit avec un
  // stride aligné 16 px (aw) : le passer au PPA comme stride source, sinon
  // toute largeur non multiple de 16 (ex. 540 après transpose) donne une image
  // tuilée.
  esp_err_t rr = ESP_OK;
  if (direct == nullptr) {
    rr = bsp::Bsp::instance().render_rgb565_to_panel(
        s_out, info.width, info.height, vzx, vzy, vzw, vzh, aw);
  }
  int64_t ta = esp_timer_get_time(); // fin PPA, avant bandeau + present
  // Le décodeur en chemin direct écrit TOUT le framebuffer, contrôles compris :
  // la page concernée re-pose sa superposition ici, sur le back-buffer courant,
  // avant que present_frame() ne le donne au scan-out.
  if (rr == ESP_OK && s_view.overlay != nullptr) {
    s_view.overlay();
  }
  int64_t tb = esp_timer_get_time();
  if (rr == ESP_OK) {
    bsp::Bsp::instance().set_direct_mode(true);
    bsp::Bsp::instance().present_frame();
  }
  int64_t t2 = esp_timer_get_time();

  // Une fois par session (remis à zéro dans start()) : dire quel chemin a été
  // retenu. Un basculement silencieux sur le PPA — go2rtc qui ne sort plus à la
  // largeur du panneau — expliquerait à lui seul un ppa=21ms de retour.
  if (!s_path_logged) {
    s_path_logged = true;
    ESP_LOGI(TAG, "%lux%lu -> zone %lux%lu@%lu,%lu : chemin %s",
             (unsigned long)info.width, (unsigned long)info.height,
             (unsigned long)vzw, (unsigned long)vzh, (unsigned long)vzx,
             (unsigned long)vzy,
             direct ? "DIRECT (JPEG -> framebuffer, sans PPA)" : "PPA");
  }

  // FPS/CPU overlay + perf, 1x/s (hors chemin chaud par frame). rnd est scindé
  // en ppa (upscale PPA) et pres (present_frame : bloque sur le vsync DSI +
  // prend le mutex LVGL, partagé avec le rafraîchissement des sprites de
  // contrôle). Les accumulateurs vivent au niveau du fichier et non en statiques
  // de fonction : start() doit pouvoir les remettre à zéro, sinon la première
  // ligne d'une session mélangeait la seconde inachevée de la précédente.
  Perf &p = s_perf;
  p.frames++;
  p.t_flip += tf1 - tf0;
  p.t_dec += t1 - t0;
  p.t_ppa += ta - t1;
  p.t_band += tb - ta;
  p.t_pres += t2 - tb;
  if (p.tw == 0) {
    p.tw = t2;
  } else if (t2 - p.tw >= 1000000) {
    // Overlay masqué (défaut) : pas de balayage uxTaskGetSystemState pour un
    // texte que update_stats jetterait de toute façon.
    if (s_view.stats != nullptr && prefs::show_stats()) {
      const sys_stats::CpuUsage cpu = sys_stats::cpu_usage_percent();
      char txt[24];
      snprintf(txt, sizeof(txt), "%lu Ca%d Cb%d R%d", (unsigned long)p.frames,
               cpu.core_a, cpu.core_b, sys_stats::psram_used_percent());
      s_view.stats(txt);
    }
    // Lecture + reset des compteurs réseau sous mutex (écrits par le sink).
    uint32_t recv, drop;
    uint64_t bytes;
    {
      sys::MutexLock lock(s_mutex);
      recv = s_recv;
      drop = s_drop;
      bytes = s_bytes;
      s_recv = s_drop = 0;
      s_bytes = 0;
    }
    // RSSI 2.4GHz : si le débit (Ko/s) plafonne alors qu'aucun CPU ne sature,
    // le suspect est le lien TCP qui s'effondre sur perte radio. RSSI faible
    // (< -70 dBm) => retransmissions => débit TCP écroulé bien sous le PHY.
    // Rafraîchi toutes les 10 s seulement : esp_wifi_sta_get_ap_info est une
    // RPC SDIO vers le C6 (timeout 5 s), jouée ici DANS la tâche de rendu et
    // sur le lien même qui porte la vidéo — un à-coup par seconde.
    static int rssi = 0;
    static uint32_t rssi_age = 0;
    if (rssi == 0 || ++rssi_age >= 10) {
      rssi = wifi_net::wifi_rssi();
      rssi_age = 0;
    }
    // Diviseur en int64 : `frames * 1000` en uint32 déborderait au-delà de
    // 4,29 M de frames (irréaliste ici, mais la conversion implicite masquait
    // le calcul en 32 bits).
    const int64_t den = (int64_t)p.frames * 1000;
    // Timeouts PPA en fin de ligne : ils sont le symptôme AVANT-COUREUR du gel
    // total (moteur SRM verrouillé, cf. bsp::ppa_video_run). Non nuls = enquête,
    // même si l'image tient encore.
    const uint32_t ppa_to_l = bsp::Bsp::instance().ppa_lvgl_timeouts();
    const uint32_t ppa_to_v = bsp::Bsp::instance().ppa_video_timeouts();
    ESP_LOGI(TAG,
             "%lu fps | recv=%lu drop=%lu %luKo/s | rssi=%ddBm | flip=%lldms "
             "dec=%lldms ppa=%lldms band=%lldms pres=%lldms (moy) | ppa_to=%lu/%lu",
             (unsigned long)p.frames, (unsigned long)recv, (unsigned long)drop,
             (unsigned long)(bytes / 1024), rssi, (long long)(p.t_flip / den),
             (long long)(p.t_dec / den), (long long)(p.t_ppa / den),
             (long long)(p.t_band / den), (long long)(p.t_pres / den),
             (unsigned long)ppa_to_l, (unsigned long)ppa_to_v);
    // Diagnostics de perte de débit/frames, désarmés par défaut
    // (sys_stats::set_perf_diag). Ils ont servi à identifier, dans l'ordre : le
    // cœur 0 saturé par le drain TCP, puis lvgl tenant le mutex 266 ms pendant
    // la composition du bandeau — deux causes qu'aucun compteur de la ligne
    // ci-dessus ne montrait.
    sys_stats::log_task_cpu(); // no-op désarmé, échantillonnage compris
    if (sys_stats::perf_diag()) {
      char owner[16] = {};
      const uint32_t hold =
          bsp::Bsp::instance().lvgl_max_hold_us(owner, sizeof(owner));
      ESP_LOGI(kLockTag, "mutex LVGL : max %lu us tenu par %s",
               (unsigned long)hold, owner[0] != '\0' ? owner : "?");
    }
    p = Perf{};
    p.tw = t2;
  }
}

// Tâche de rendu (core 1) : consomme la dernière frame JPEG de la mailbox,
// décode (HW) + affiche. Découplée du réseau -> pas d'accumulation de retard.
void render_task(void *arg) {
  (void)arg;
  while (s_render_run.load(std::memory_order_relaxed)) {
    if (xSemaphoreTake(s_sem, pdMS_TO_TICKS(200)) != pdTRUE) {
      banner_lost(); // no-op tant que le flux tient la cadence
      continue;
    }
    int idx;
    size_t len;
    {
      sys::MutexLock lock(s_mutex);
      idx = s_ready;
      s_ready = -1;
      s_rendering = idx;
      len = (idx >= 0) ? s_mb_len[idx] : 0;
    }
    if (idx >= 0 && len > 0 && s_render_run.load(std::memory_order_relaxed)) {
      banner_clear();
      decode_render(s_mb[idx], len);
    }
    {
      sys::MutexLock lock(s_mutex);
      s_rendering = -1;
    }
  }
  xSemaphoreGive(s_render_stopped);
  vTaskDelete(nullptr);
}

void free_mailbox() {
  if (s_mb_abandoned) {
    return; // slots encore écrits par une tâche réseau non sortie
  }
  for (auto &slot : s_mb) {
    heap_caps_free(slot);
    slot = nullptr;
  }
  s_mb_cap = 0;
}

// Sortie du décodeur JPEG (chemin PPA uniquement) : jusqu'à 800x640x2 = 1 Mo,
// et davantage si go2rtc change de résolution. Même raison de la rendre que la
// mailbox — la PSRAM sert aux Lottie de l'écran météo hors session.
void free_decoder_output() {
  if (s_out != nullptr) {
    free(s_out); // jpeg_alloc_decoder_mem -> free (pas heap_caps_free)
    s_out = nullptr;
  }
  s_out_cap = 0;
}

} // namespace

void init(StreamingProbe is_streaming) {
  s_is_streaming = is_streaming;
  jpeg_decode_engine_cfg_t eng = {};
  eng.timeout_ms = 2000;
  if (jpeg_new_decoder_engine(&eng, &s_dec) != ESP_OK) {
    ESP_LOGE(TAG, "création décodeur JPEG HW échouée");
    s_dec = nullptr;
  }
}

bool start(const char *url, size_t slot_bytes, const View &view) {
  if (s_dec == nullptr) {
    ESP_LOGE(TAG, "décodeur JPEG non initialisé");
    return false;
  }
  if (s_mutex == nullptr) {
    s_mutex = xSemaphoreCreateMutex();
    s_sem = xSemaphoreCreateBinary();
    s_render_stopped = xSemaphoreCreateBinary();
    if (s_mutex == nullptr || s_sem == nullptr || s_render_stopped == nullptr) {
      ESP_LOGE(TAG, "création des sémaphores échouée");
      return false;
    }
  }
  if (s_mb_abandoned) {
    ESP_LOGE(TAG, "mailbox abandonnée (tâche réseau non sortie) : "
                  "démarrage vidéo refusé jusqu'au redémarrage");
    return false;
  }
  s_view = view;
  s_frame_cap = slot_bytes;
  // Les deux pages caméra n'ont pas la même résolution donc pas la même taille
  // de slot : libérer avant de réallouer si la session précédente en demandait
  // une autre.
  if (s_mb_cap != slot_bytes) {
    free_mailbox();
  }
  // Slots alignés 64 : la tâche réseau y accumule le JPEG qui est ensuite
  // passé tel quel en entrée du décodeur matériel (DMA).
  for (auto &slot : s_mb) {
    if (slot == nullptr) {
      slot = static_cast<uint8_t *>(
          heap_caps_aligned_alloc(64, slot_bytes, MALLOC_CAP_SPIRAM));
      if (slot == nullptr) {
        ESP_LOGE(TAG, "alloc mailbox (%u o) échouée", (unsigned)slot_bytes);
        free_mailbox();
        return false;
      }
    }
  }
  s_mb_cap = slot_bytes;

  s_ready = -1;
  s_rendering = -1;
  s_writing = -1;
  s_last = -1;
  s_path_logged = false;
  s_lost = false;
  s_perf = Perf{};
  // Le compte à rebours du bandeau part de l'ouverture de session, pas d'une
  // frame : sinon un flux qui ne démarre jamais n'afficherait jamais l'erreur.
  s_last_frame_us = esp_timer_get_time();
  s_render_run.store(true, std::memory_order_relaxed);
  // Rendu sur core 1 : décodage HW + PPA hors du core réseau. Pile modeste
  // (décodage/PPA n'utilisent pas de grosse pile).
  if (xTaskCreatePinnedToCore(render_task, "mjpeg_rnd", 4096, nullptr, 4,
                              nullptr, 1) != pdPASS) {
    ESP_LOGE(TAG, "création mjpeg_rnd échouée");
    s_render_run.store(false, std::memory_order_relaxed);
    free_mailbox();
    return false;
  }

  mjpeg::Sink sink = {};
  sink.begin_frame = sink_begin;
  sink.end_frame = sink_end;
  sink.abort_frame = sink_abort;
  sink.ctx = nullptr;
  if (!mjpeg::start(url, sink)) {
    stop();
    return false;
  }
  return true;
}

void stop() {
  // Arrête le réseau EN PREMIER : passé ce point plus aucun slot n'est emprunté
  // — à condition que la tâche soit réellement sortie, d'où le retour testé.
  const bool net_joined = mjpeg::stop();


  bool render_joined = true;
  if (s_render_run.load(std::memory_order_relaxed)) {
    s_render_run.store(false, std::memory_order_relaxed);
    xSemaphoreGive(s_sem); // débloque la tâche si en attente
    // 3 s et non 1 : decode_render peut à lui seul dépasser la seconde
    // (wait_frame_flip 100 ms + décodage + PPA, puis present_frame bloqué sur
    // le mutex LVGL que la composition du bandeau tient jusqu'à 266 ms).
    render_joined =
        xSemaphoreTake(s_render_stopped, pdMS_TO_TICKS(3000)) == pdTRUE;
  }
  // Pas de bandeau orphelin sur l'écran d'accueil après un retour manuel.
  if (s_lost) {
    s_lost = false;
    ui::hide_error();
  }
  // s_last inclus : les slots partent au tas juste en dessous, plus rien ne doit
  // pouvoir les désigner (capture_snapshot s'appelle AVANT stop()).
  s_ready = s_rendering = s_writing = s_last = -1;

  // 256 Ko + la sortie du décodeur rendus au tas entre deux sessions caméra :
  // la PSRAM sert aussi aux écrans LVGL (Lottie météo) quand la vidéo est
  // arrêtée. MAIS un join raté veut dire qu'une tâche lit encore ces buffers —
  // celle du réseau écrit dans un slot de mailbox, celle du rendu passe la
  // mailbox et s_out au DMA du JPEG matériel. On préfère fuir la mémoire
  // plutôt que la corrompre.
  if (net_joined && render_joined) {
    free_mailbox();
    free_decoder_output();
  } else {
    // Collant : la tâche survivante garde le droit d'écrire dans ces buffers
    // pour toute la durée de vie restante du firmware. Sans ce drapeau, la
    // session suivante — un simple passage de la page caméra V1 à la page HD,
    // qui change la taille de slot — appelait free_mailbox() sur de la PSRAM
    // encore sous memcpy.
    s_mb_abandoned = true;
    ESP_LOGE(TAG,
             "arrêt incomplet (réseau=%d rendu=%d) : buffers abandonnés "
             "définitivement",
             (int)net_joined, (int)render_joined);
  }
}

bool capture_snapshot(uint8_t *dst, size_t cap, size_t *out_len) {
  if (dst == nullptr || s_mutex == nullptr || s_mb_abandoned) {
    return false;
  }
  sys::MutexLock lock(s_mutex);
  // Ordre de préférence : la frame en cours de rendu, sinon celle qui vient
  // d'être publiée, sinon la dernière publiée (cas normal en fin d'appel, cf.
  // s_last). Copie sous le mutex : la tâche réseau ne peut donc pas se voir
  // prêter ce slot au milieu du memcpy.
  const int idx = (s_rendering >= 0) ? s_rendering
                  : (s_ready >= 0)   ? s_ready
                                     : s_last;
  if (idx < 0 || idx == s_writing || s_mb[idx] == nullptr) {
    return false;
  }
  const size_t len = s_mb_len[idx];
  if (len == 0 || len > cap) {
    return false;
  }
  memcpy(dst, s_mb[idx], len);
  if (out_len != nullptr) {
    *out_len = len;
  }
  return true;
}

bool render_still(const uint8_t *jpeg, size_t len) {
  if (s_dec == nullptr || jpeg == nullptr || len == 0) {
    return false;
  }
  if (streaming()) {
    ESP_LOGW(TAG, "render_still ignoré : session en cours");
    return false;
  }

  jpeg_decode_picture_info_t info = {};
  if (jpeg_decoder_get_info(jpeg, len, &info) != ESP_OK) {
    ESP_LOGE(TAG, "capture illisible (get_info)");
    return false;
  }

  // Même contrainte d'alignement 16 px que le chemin live : c'est le décodeur
  // matériel qui l'impose, pas le PPA.
  const uint32_t aw = (info.width + 15) & ~15u;
  const uint32_t ah = (info.height + 15) & ~15u;
  const size_t need = (size_t)aw * ah * 2; // RGB565
  if (s_out_cap < need) {
    free_decoder_output();
    jpeg_decode_memory_alloc_cfg_t oc = {JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    size_t got = 0;
    s_out = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(need, &oc, &got));
    if (s_out == nullptr) {
      ESP_LOGE(TAG, "alloc sortie capture %u o échouée", (unsigned)need);
      return false;
    }
    s_out_cap = got;
  }

  jpeg_decode_cfg_t dcfg = {};
  dcfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  dcfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR; // cf. decode_render
  dcfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
  uint32_t out_size = 0;
  const esp_err_t derr = jpeg_decoder_process(s_dec, &dcfg, jpeg, len, s_out,
                                              s_out_cap, &out_size);
  if (derr != ESP_OK) {
    ESP_LOGE(TAG, "décodage capture : %s", esp_err_to_name(derr));
    return false;
  }

  // La bascule armée par la dernière frame de la session peut ne pas encore
  // avoir été prise : écrire avant, c'est repeindre le buffer AFFICHÉ.
  bsp::Bsp::instance().wait_frame_flip();
  const esp_err_t rr = bsp::Bsp::instance().render_rgb565_to_panel(
      s_out, info.width, info.height, 0, 0,
      (uint32_t)bsp::Bsp::kLcdHRes, (uint32_t)bsp::Bsp::kLcdVRes, aw);
  if (rr != ESP_OK) {
    ESP_LOGE(TAG, "rendu capture : %s", esp_err_to_name(rr));
    return false;
  }
  bsp::Bsp::instance().present_frame();
  ESP_LOGI(TAG, "capture affichée (%lux%lu)", (unsigned long)info.width,
           (unsigned long)info.height);
  return true;
}

void release_still() {
  if (streaming()) {
    return; // s_out appartient au chemin chaud tant qu'une session tourne
  }
  free_decoder_output();
}

} // namespace video_mjpeg
