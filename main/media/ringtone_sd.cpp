#include "media/ringtone_sd.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media/ringtone.h"
#include "media/wav_parse.h"
#include "sys/i18n.h"
#include "sys/prefs.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <strings.h> // strcasecmp : pas dans <cstring>

using i18n::Str;

namespace ringtone_sd {
namespace {

constexpr const char *TAG = "ringtone_sd";
constexpr const char *kRoot = "/sdcard";

// Fenêtre d'en-tête : couvre largement les chunks LIST/INFO que ffmpeg insère.
constexpr size_t kHeaderBytes = 4096;

// Plafond aligné sur kMaxRingUs (ringtone.cpp) : 30 s à 8 kHz. Au-delà, les
// échantillons ne seraient jamais joués — la sonnerie s'auto-désarme avant.
constexpr size_t kMaxSamples = 240000;
constexpr uint32_t kSampleRate = 8000;

Status s_status = Status::kAbsent;
// Dimensionné au pire cas : un nom de kNameMax caractères suivi du plus long
// suffixe. Trop juste, -Werror=format-truncation refuserait de compiler.
char s_summary[128] = "sonnerie interne";

// Bloc PSRAM de la source installée. Mémorisé pour que load() puisse être
// rappelé — la sortie du mode clé USB le fait, pour prendre en compte le
// ring.wav qui vient d'être déposé. Sans ça, chaque appel fuirait l'ancien
// bloc (jusqu'à 480 Ko).
int16_t *s_pcm = nullptr;

bool has_wav_suffix(const char *name) {
  const size_t n = strlen(name);
  if (n < 5) {
    return false; // il faut au moins un caractère avant ".wav"
  }
  return strcasecmp(name + n - 4, ".wav") == 0;
}

} // namespace

void load() {
  // Copie locale du nom : tout ce qui suit le formate dans des chemins et des
  // messages, et prefs le rendrait à nouveau à chaque appel.
  char name[kNameMax];
  snprintf(name, sizeof name, "%s", prefs::ring_file());

  s_status = Status::kAbsent;
  snprintf(s_summary, sizeof s_summary, "%s",
           i18n::t(Str::kRingBuiltIn));

  // Rendre l'ancienne source AVANT d'en installer une autre. Ordre imposé :
  // désarmer, désinstaller, ATTENDRE, puis libérer. mix_into() sort en tête
  // quand la sonnerie n'est pas armée, mais un mix déjà entamé dans aud_render
  // lit encore le bloc — le libérer sous ses pieds donnerait du bruit blanc
  // dans le haut-parleur, ou pire. Un bloc de 8 kHz fait au plus quelques
  // dizaines de ms ; 50 ms couvrent largement. Ce chemin n'est jamais chaud
  // (boot, sortie du mode clé USB, changement de fichier au doigt).
  if (s_pcm != nullptr) {
    ringtone::stop();
    ringtone::clear_source();
    vTaskDelay(pdMS_TO_TICKS(50));
    heap_caps_free(s_pcm);
    s_pcm = nullptr;
  }

  if (name[0] == '\0') {
    // Choix explicite de l'utilisateur : pas de fichier, mélodie synthétisée.
    ESP_LOGI(TAG, "sonnerie interne sélectionnée");
    return;
  }

  char path[16 + kNameMax];
  snprintf(path, sizeof path, "%s/%s", kRoot, name);

  FILE *f = fopen(path, "rb");
  if (f == nullptr) {
    snprintf(s_summary, sizeof s_summary,
             i18n::t(Str::kRingFileMissingFmt),
             name);
    ESP_LOGI(TAG, "%s absent : sonnerie interne conservée", path);
    return;
  }

  // En-tête en PSRAM et non sur la pile : app_main ne dispose que de quelques
  // kilo-octets, 4 Ko de tampon local la feraient déborder.
  uint8_t *hdr =
      static_cast<uint8_t *>(heap_caps_malloc(kHeaderBytes, MALLOC_CAP_SPIRAM));
  if (hdr == nullptr) {
    fclose(f);
    s_status = Status::kIoError;
    snprintf(s_summary, sizeof s_summary,
             i18n::t(Str::kRingFileUnreadableFmt),
             name);
    ESP_LOGE(TAG, "allocation du tampon d'en-tête refusée");
    return;
  }

  const size_t got = fread(hdr, 1, kHeaderBytes, f);
  wav::Info info;
  const wav::Error err = wav::parse(hdr, got, info);
  heap_caps_free(hdr);

  if (err != wav::Error::kOk) {
    fclose(f);
    s_status = Status::kBadFormat;
    snprintf(s_summary, sizeof s_summary,
             i18n::t(Str::kRingFileRejectedFmt), name,
             wav::error_text(err));
    // Le détail chiffré ne tient pas sur le label de la page Configuration : il
    // vit ici, dans le journal SD relisable depuis la page Debug.
    ESP_LOGW(TAG, "%s rejeté : %s (%lu Hz, %u voie(s), %u bits)", path,
             wav::error_text(err), (unsigned long)info.sample_rate,
             (unsigned)info.channels, (unsigned)info.bits);
    return;
  }

  size_t samples = info.data_bytes / sizeof(int16_t);
  const bool truncated = samples > kMaxSamples;
  if (truncated) {
    samples = kMaxSamples;
  }

  int16_t *pcm = static_cast<int16_t *>(
      heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  if (pcm == nullptr) {
    fclose(f);
    s_status = Status::kIoError;
    snprintf(s_summary, sizeof s_summary,
             i18n::t(Str::kRingFileTooBigFmt),
             name);
    ESP_LOGE(TAG, "PSRAM insuffisante pour %u échantillons", (unsigned)samples);
    return;
  }

  if (fseek(f, (long)info.data_offset, SEEK_SET) != 0 ||
      fread(pcm, sizeof(int16_t), samples, f) != samples) {
    fclose(f);
    heap_caps_free(pcm);
    s_status = Status::kIoError;
    snprintf(s_summary, sizeof s_summary,
             i18n::t(Str::kRingFileUnreadableFmt),
             name);
    ESP_LOGE(TAG, "lecture de %s incomplète", path);
    return;
  }
  fclose(f);

  // Buffer JAMAIS libéré : sa durée de vie est celle du programme, comme les
  // tables A-law. ringtone n'en garde qu'un pointeur.
  s_pcm = pcm;
  ringtone::set_source(pcm, samples);
  const uint32_t ms =
      static_cast<uint32_t>(samples) * 1000u / kSampleRate; // < 4,3e9, sûr
  s_status = truncated ? Status::kTooBig : Status::kLoaded;
  if (truncated) {
    snprintf(s_summary, sizeof s_summary,
             i18n::t(Str::kRingFileCutFmt), name);
  } else {
    snprintf(s_summary, sizeof s_summary, "%s : %lu,%lu s", name,
             (unsigned long)(ms / 1000u), (unsigned long)((ms % 1000u) / 100u));
  }
  ESP_LOGI(TAG, "%s chargé : %lu ms, %u Ko en PSRAM%s", path,
           (unsigned long)ms, (unsigned)(samples * sizeof(int16_t) / 1024u),
           truncated ? " (tronqué à 30 s)" : "");
}

size_t list_wav(char out[][kNameMax], size_t max) {
  if (out == nullptr || max == 0) {
    return 0;
  }
  DIR *dir = opendir(kRoot);
  if (dir == nullptr) {
    return 0; // carte absente ou démontée : liste vide, pas une erreur
  }
  size_t n = 0;
  struct dirent *ent = nullptr;
  while ((ent = readdir(dir)) != nullptr && n < max) {
    // '.' en tête : ".", ".." et surtout les doublons « ._nom.wav » que macOS
    // sème sur toute carte FAT qu'il a montée. Ce ne sont pas des WAV.
    if (ent->d_name[0] == '.' || !has_wav_suffix(ent->d_name)) {
      continue;
    }
    if (strlen(ent->d_name) >= kNameMax) {
      continue; // nom que la NVS ne saurait pas mémoriser : ne pas le proposer
    }
    // Insertion triée : la liste est bornée à kMaxFiles, un tri par insertion
    // y coûte moins qu'un qsort et évite un tampon de travail.
    size_t i = n;
    while (i > 0 && strcasecmp(out[i - 1], ent->d_name) > 0) {
      snprintf(out[i], kNameMax, "%s", out[i - 1]);
      i--;
    }
    snprintf(out[i], kNameMax, "%s", ent->d_name);
    n++;
  }
  closedir(dir);
  return n;
}

Status status() { return s_status; }

const char *summary() { return s_summary; }

} // namespace ringtone_sd
