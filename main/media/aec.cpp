#include "media/aec.h"

#include "esp_aec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <cstring>

namespace aec {
namespace {

constexpr const char *TAG = "aec";
// Seul taux accepté par aec_create() (header esp-sr). 8 kHz a été mesuré :
// les modes qui le prétendent (VOIP) coûtent 60 à 450 % d'un cœur ici.
constexpr int kAecRate = 16000;
// Valeur recommandée par le header esp-sr, celle des mesures.
constexpr int kFilterLength = 4;
// Planchers de SRAM interne. Mesuré : une session vidéo seule laisse ~27 Ko,
// et elle a tenu à 17 Ko avec l'AEC. En dessous, ce sont le SDIO d'esp_hosted
// et lwip qui manquent : flux vidéo perdu puis plantage. Un AEC n'en vaut pas
// la peine — mieux vaut un visiteur qui s'entend qu'un appel coupé.
constexpr size_t kMinInternalBefore = 20 * 1024;
constexpr size_t kMinInternalAfter = 14 * 1024;

aec_handle_t *s_h = nullptr;
size_t s_blk8 = 0; // un bloc AEC, en échantillons 8 kHz (chunk / 2)

// Blocs 16 kHz passés à aec_process : alignés comme l'exige le header.
int16_t *s_mic16 = nullptr;
int16_t *s_ref16 = nullptr;
int16_t *s_out16 = nullptr;

// Files 8 kHz : entrée en attente d'un bloc complet, sortie en attente d'être
// rendue. Taille d'une file = un bloc + une trame, cf. invariant de process().
int16_t *s_in_mic = nullptr;
int16_t *s_in_ref = nullptr;
int16_t *s_out = nullptr;
size_t s_in_len = 0;
size_t s_out_len = 0;

// Dernier échantillon de chaque bloc précédent : l'interpolation ×2 doit
// enjamber la frontière de bloc, sinon un clic toutes les 32 ms.
int16_t s_prev_mic = 0;
int16_t s_prev_ref = 0;

int16_t *alloc16(size_t samples) {
  return static_cast<int16_t *>(heap_caps_aligned_calloc(
      16, samples, sizeof(int16_t), MALLOC_CAP_SPIRAM));
}

// ×2 par interpolation linéaire : la voix G.711 n'a rien au-dessus de 4 kHz,
// un filtre plus raide ne servirait que la mesure, pas l'oreille.
void up2(const int16_t *in, int16_t *out, size_t n, int16_t &prev) {
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = (int16_t)(((int32_t)prev + in[i]) / 2);
    out[2 * i + 1] = in[i];
    prev = in[i];
  }
}

// ÷2 par moyenne de paires : passe-bas grossier suffisant avant décimation
// d'un signal déjà limité à 4 kHz par le CODEC.
void down2(const int16_t *in, int16_t *out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    out[i] = (int16_t)(((int32_t)in[2 * i] + in[2 * i + 1]) / 2);
  }
}

} // namespace

bool start(bool high_perf) {
  if (s_h != nullptr) {
    return true;
  }
  const size_t int0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (int0 < kMinInternalBefore) {
    ESP_LOGW(TAG, "SRAM interne trop basse (%u o < %u) : micro sans AEC",
             (unsigned)int0, (unsigned)kMinInternalBefore);
    return false;
  }
  // caps PSRAM : aec_create() simple alloue en SRAM interne (8,5 à 12,7 Ko
  // mesurés), pile dans la réserve dont le SDIO d'esp_hosted et lwip ont besoin
  // pendant un appel. La bibliothèque ne documente pas ce qu'elle garde quand
  // même en interne : le delta est journalisé ci-dessous.
  aec_config_t cfg = {};
  cfg.mic_num = 1;
  cfg.ref_num = 1;
  cfg.out_num = 1;
  cfg.filter_length = kFilterLength;
  cfg.sample_rate = kAecRate;
  cfg.caps = MALLOC_CAP_SPIRAM;
  cfg.mode = high_perf ? AEC_MODE_FD_HIGH_PERF : AEC_MODE_FD_LOW_COST;
  cfg.nlp_level = AEC_NLP_LEVEL_AGGR; // défaut documenté
  s_h = aec_create_from_config(&cfg);
  if (s_h == nullptr) {
    ESP_LOGE(TAG, "aec_create a échoué (interne libre=%u) : micro sans AEC",
             (unsigned)int0);
    return false;
  }
  const size_t int1 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (int1 < kMinInternalAfter) {
    ESP_LOGW(TAG, "SRAM interne après AEC %u o < %u : AEC abandonné",
             (unsigned)int1, (unsigned)kMinInternalAfter);
    aec_destroy(s_h);
    s_h = nullptr;
    return false;
  }
  const int chunk = aec_get_chunksize(s_h);
  s_blk8 = chunk > 0 ? (size_t)chunk / 2 : 0;
  const size_t cap = s_blk8 + kMaxFrame;
  s_mic16 = alloc16((size_t)chunk);
  s_ref16 = alloc16((size_t)chunk);
  s_out16 = alloc16((size_t)chunk);
  s_in_mic = alloc16(cap);
  s_in_ref = alloc16(cap);
  s_out = alloc16(cap);
  if (s_blk8 == 0 || !s_mic16 || !s_ref16 || !s_out16 || !s_in_mic ||
      !s_in_ref || !s_out) {
    ESP_LOGE(TAG, "tampons AEC non alloués (chunk=%d) : micro sans AEC", chunk);
    stop();
    return false;
  }
  // Un bloc de silence d'avance en sortie : c'est ce qui garantit qu'une trame
  // est toujours disponible (cf. process), au prix d'un bloc de latence.
  s_out_len = s_blk8;
  s_in_len = 0;
  s_prev_mic = s_prev_ref = 0;
  ESP_LOGI(TAG,
           "AEC %s actif : bloc %u éch. 8 kHz, SRAM interne prise %d o, "
           "reste %u o",
           high_perf ? "FD_HIGH_PERF" : "FD_LOW_COST", (unsigned)s_blk8,
           (int)(int0 - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  return true;
}

void stop() {
  if (s_h != nullptr) {
    aec_destroy(s_h);
    s_h = nullptr;
  }
  heap_caps_free(s_mic16);
  heap_caps_free(s_ref16);
  heap_caps_free(s_out16);
  heap_caps_free(s_in_mic);
  heap_caps_free(s_in_ref);
  heap_caps_free(s_out);
  s_mic16 = s_ref16 = s_out16 = s_in_mic = s_in_ref = s_out = nullptr;
  s_in_len = s_out_len = s_blk8 = 0;
}

bool active() { return s_h != nullptr; }

// Invariant : après chaque appel, s_in_len + s_out_len == s_blk8 (autant
// d'échantillons sortent qu'il en entre, et l'amorçage en a mis un bloc en
// sortie). Avant la sortie, s_out_len vaut donc s_blk8 + n - s_in_len, avec
// s_in_len < s_blk8 une fois les blocs traités : toujours > n. Les files ne
// dépassent jamais un bloc + une trame.
void process(const int16_t *mic, const int16_t *ref, int16_t *out, size_t n) {
  if (s_h == nullptr || n > kMaxFrame) {
    // memmove : mic_session appelle process() EN PLACE (out == mic), et un
    // memcpy sur zones confondues est un comportement indéfini.
    if (out != mic) {
      memmove(out, mic, n * sizeof(int16_t));
    }
    return;
  }
  memcpy(s_in_mic + s_in_len, mic, n * sizeof(int16_t));
  memcpy(s_in_ref + s_in_len, ref, n * sizeof(int16_t));
  s_in_len += n;

  while (s_in_len >= s_blk8) {
    up2(s_in_mic, s_mic16, s_blk8, s_prev_mic);
    up2(s_in_ref, s_ref16, s_blk8, s_prev_ref);
    aec_process(s_h, s_mic16, s_ref16, s_out16);
    down2(s_out16, s_out + s_out_len, s_blk8);
    s_out_len += s_blk8;
    s_in_len -= s_blk8;
    memmove(s_in_mic, s_in_mic + s_blk8, s_in_len * sizeof(int16_t));
    memmove(s_in_ref, s_in_ref + s_blk8, s_in_len * sizeof(int16_t));
  }

  memcpy(out, s_out, n * sizeof(int16_t));
  s_out_len -= n;
  memmove(s_out, s_out + n, s_out_len * sizeof(int16_t));
}

} // namespace aec
