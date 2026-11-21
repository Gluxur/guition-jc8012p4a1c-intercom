#include "app/audio_pipeline.h"

#include "bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "media/g711.h"
#include "media/ringtone.h"

#include <cstring>

namespace audio_pipeline {
namespace {

constexpr const char *TAG = "AUDIO_DBG";

// --- Pipeline audio découplé ------------------------------------------------
// esp_codec_dev_write() est bloquant (I2S DMA) : on ne veut jamais bloquer
// esp_peer_main_loop() dessus.
// FIFO free-list (et non mailbox "last wins" comme la vidéo) : go2rtc envoie
// des chunks de 1024 échantillons = 128ms pile au rythme de lecture — zéro
// marge. Le moindre jitter écrasait le chunk non joué -> trous audibles.
// 4 slots = jusqu'à ~512ms absorbés, latence bornée, aucun drop en régime
// nominal. Slots en PSRAM (gros buffers, règle projet).
constexpr size_t kSlotSamples = 1920; // 240ms @ 8kHz, > chunk go2rtc max observé
constexpr int kSlotCount = 4;

// Frame silencieuse jouée quand la sonnerie sonne mais qu'aucun slot audio
// distant n'arrive à temps : garantit la cadence du ding-dong même si le VTO
// n'envoie pas d'audio. 30 ms @ 8 kHz. Statique (hors pile de la tâche).
// 240 = exactement un descripteur DMA I2S (I2S_CHANNEL_DEFAULT_CONFIG :
// dma_frame_num = 240). C'est ce qui rend l'amorçage ci-dessous nécessaire.
constexpr size_t kSilenceSamples = 240;

// Frames poussées d'affilée au démarrage de la sonnerie hors streaming.
// Sans ça : le DMA I2S est VIDE au clic (personne ne l'alimente à l'idle) et
// la boucle ci-dessous ne peut prendre qu'UN descripteur d'avance par tour,
// son avance nette valant (30 ms - durée réelle du timeout de 3 ticks), soit
// quelques ms à 100 Hz de tick. Remplir les 180 ms du DMA prend donc ~1 s,
// pendant laquelle il tourne à ras : au moindre glissement du réveil de la
// tâche l'I2S n'a rien à émettre et reboucle son dernier descripteur -> son
// haché sur la première seconde de sonnerie, puis propre une fois le coussin
// constitué. 6 = la profondeur DMA complète ; la rafale s'auto-limite car
// play_pcm bloque dès que les descripteurs sont pleins.
constexpr int kPrimeFrames = 6;

int16_t *s_slots[kSlotCount] = {};
size_t s_slot_len[kSlotCount] = {};
QueueHandle_t s_play_q = nullptr; // indexes de slots prêts à jouer
QueueHandle_t s_free_q = nullptr; // indexes de slots libres
uint32_t s_dropped = 0;
int16_t s_silence[kSilenceSamples];
StreamingProbe s_is_streaming = nullptr;

bool streaming() { return s_is_streaming != nullptr && s_is_streaming(); }

// Fabrique et joue une frame de sonnerie seule (silence + mix). À n'appeler que
// depuis aud_render : mix_into() n'a pas de verrou sur son curseur.
void play_ring_frame() {
  memset(s_silence, 0, sizeof(s_silence));
  ringtone::mix_into(s_silence, kSilenceSamples);
  bsp::Bsp::instance().play_pcm(s_silence, kSilenceSamples);
}

void render_task(void *arg) {
  (void)arg;
  int played = 0;
  bool was_ringing = false;
  bool was_streaming = false;
  int flush_frames = 0; // frames de silence restant à pousser après l'arrêt
  // Dernier slot DISTANT joué. Pendant une sonnerie avec flux ouvert, les
  // trames « sonnerie seule » ne doivent combler qu'une absence d'audio : elles
  // s'intercalaient sinon entre les slots distants (30 ms de plus à chaque
  // timeout), la production dépassait la consommation et le retard ainsi
  // accumulé restait après le décroché.
  int64_t last_remote_us = 0;
  constexpr int64_t kRemoteGapUs = 300000;
  while (true) {
    bool ringing = ringtone::is_ringing();
    bool is_streaming = streaming();
    // Transition sonnerie -> silence : le DMA I2S reboucle son dernier buffer
    // (fragment de ding fort) tant qu'on ne le réalimente pas -> buzz au
    // raccrochage/retour. On programme quelques frames de silence pour le
    // purger proprement.
    // 8 frames (~240 ms) : couvre toute la profondeur du DMA I2S par défaut
    // (~1440 échantillons / 180 ms) pour écraser le dernier ding en entier.
    // Même reloop DMA à l'arrêt du flux (retour page caméra) : la dernière
    // frame audio distante reste rebouclée sans réalimentation -> son
    // résiduel. Purger avec du silence comme pour la sonnerie.
    if ((was_ringing && !ringing) || (was_streaming && !is_streaming)) {
      flush_frames = 8;
    }
    // Front montant de la sonnerie hors streaming : le DMA I2S est vide, on le
    // sature d'un coup (cf. kPrimeFrames). En streaming, les slots distants
    // (1024 éch. = 128 ms) le remplissent déjà : amorcer ne ferait qu'ajouter
    // 180 ms de retard à la voix.
    if (!was_ringing && ringing && !is_streaming) {
      for (int i = 0; i < kPrimeFrames; i++) {
        play_ring_frame();
      }
    }
    was_ringing = ringing;
    was_streaming = is_streaming;

    int idx;
    // Poll court tant que la sonnerie sonne ou qu'il reste du silence à purger
    // ; sinon blocage classique en attente d'un slot distant.
    TickType_t wait =
        (ringing || flush_frames > 0) ? pdMS_TO_TICKS(30) : portMAX_DELAY;
    if (xQueueReceive(s_play_q, &idx, wait) != pdTRUE) {
      // Pas de slot distant à temps : maintenir la cadence audio localement.
      if (ringing && (!is_streaming ||
                      esp_timer_get_time() - last_remote_us > kRemoteGapUs)) {
        // La sonnerie doit partir dès l'armement (sonnette HA), sans attendre
        // que le WebRTC soit établi : en streaming, elle ne comble que
        // l'absence d'audio distant (premiers paquets pas encore arrivés).
        play_ring_frame();
      } else if (flush_frames > 0) {
        memset(s_silence, 0, sizeof(s_silence));
        bsp::Bsp::instance().play_pcm(s_silence, kSilenceSamples);
        flush_frames--;
      }
      continue;
    }
    // idx < 0 : sentinelle de réveil (armement sonnerie) postée par wake()
    // pour débloquer le portMAX_DELAY. Pas un vrai slot.
    if (idx < 0) {
      continue;
    }
    // Rattrapage : la lecture est cadencée par le DAC et l'arrivée par le VTO,
    // à débit égal. Toute rafale réseau (trou Wi-Fi puis rattrapage) laissait
    // 2-3 slots en attente, soit +256 à 384 ms de retard FIGÉS jusqu'à la fin
    // de l'appel (aucun drop avant 4 slots pleins), le son passant derrière la
    // vidéo « last frame wins ». Plus d'un slot d'avance en file : on jette le
    // plus ancien. Un trou de 128 ms une fois vaut mieux qu'un décalage
    // permanent.
    if (uxQueueMessagesWaiting(s_play_q) >= 2) {
      xQueueSend(s_free_q, &idx, 0);
      s_dropped++;
      continue;
    }
    if (streaming()) {
      last_remote_us = esp_timer_get_time();
      // mix_into est un no-op si la sonnerie n'est pas armée.
      ringtone::mix_into(s_slots[idx], s_slot_len[idx]);
      esp_err_t err =
          bsp::Bsp::instance().play_pcm(s_slots[idx], s_slot_len[idx]);
      flush_frames = 0; // audio réel réécrit le DMA : purge inutile
      played++;
      if (played % 50 == 1) {
        // LOGD : chemin par trame (règle projet), compilé hors en release.
        ESP_LOGD(TAG, "play_pcm #%d samples=%u err=%s", played,
                 (unsigned)s_slot_len[idx], esp_err_to_name(err));
      }
    }
    xQueueSend(s_free_q, &idx, 0);
  }
}

} // namespace

bool init(StreamingProbe is_streaming) {
  s_is_streaming = is_streaming;
  s_play_q = xQueueCreate(kSlotCount, sizeof(int));
  s_free_q = xQueueCreate(kSlotCount, sizeof(int));
  if (s_play_q == nullptr || s_free_q == nullptr) {
    ESP_LOGE(TAG, "création des files audio échouée");
    return false;
  }
  int usable = 0;
  for (int i = 0; i < kSlotCount; i++) {
    s_slots[i] = static_cast<int16_t *>(
        heap_caps_malloc(kSlotSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (s_slots[i] != nullptr) {
      xQueueSend(s_free_q, &i, 0);
      usable++;
    }
  }
  if (usable == 0) {
    ESP_LOGE(TAG, "aucun slot PCM alloué : audio distant désactivé");
    return false;
  }

  // Prio 6 > rendu vidéo (4) : la tâche audio ne fait que nourrir le DMA I2S
  // (CPU négligeable) mais est critique en latence — affamée par le rendu
  // (44ms/frame sur core 1), l'I2S sous-alimentait -> son haché.
  if (xTaskCreatePinnedToCore(render_task, "aud_render", 4096, nullptr, 6,
                              nullptr, 1) != pdPASS) {
    ESP_LOGE(TAG, "création aud_render échouée");
    return false;
  }
  return true;
}

void submit_alaw(const uint8_t *alaw, size_t count) {
  if (s_play_q == nullptr || s_free_q == nullptr) {
    return;
  }
  if (count > kSlotSamples) {
    count = kSlotSamples;
  }
  int idx;
  // 0 tick : jamais bloquer le thread réseau. Plus de slot libre = la
  // lecture I2S est en retard ; on jette le chunk le plus récent.
  if (xQueueReceive(s_free_q, &idx, 0) != pdTRUE) {
    if ((++s_dropped % 20) == 1) {
      ESP_LOGW(TAG, "audio queue full, dropped=%lu", (unsigned long)s_dropped);
    }
    return;
  }
  for (size_t i = 0; i < count; i++) {
    s_slots[idx][i] = g711::alaw_to_pcm16(alaw[i]);
  }
  s_slot_len[idx] = count;
  xQueueSend(s_play_q, &idx, 0);
}

void wake() {
  if (s_play_q == nullptr) {
    return;
  }
  int sentinel = -1;
  xQueueSend(s_play_q, &sentinel, 0);
}

} // namespace audio_pipeline
