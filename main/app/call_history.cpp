#include "app/call_history.h"

#include "bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "app_config.h"
#include "app/video_mjpeg.h"
#include "sys/usb_msc.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

namespace call_history {
namespace {

constexpr const char *TAG = "call_hist";

constexpr char kNamespace[] = "call_hist";
constexpr char kMissedKey[] = "missed";
constexpr char kSlotKey[] = "slot";
constexpr char kLastKey[] = "last";

// Même ordre de grandeur qu'un slot de mailbox MJPEG (camera_session::kSlotHd) :
// la capture est la frame telle qu'elle est arrivée, jamais réencodée.
constexpr size_t kShotMax = 320 * 1024;

// La capture n'est pas toujours disponible du premier coup : la tâche réseau
// peut tenir le slot au moment précis de l'appel (accumulation en cours).
constexpr int kCaptureTries = 3;
constexpr uint32_t kCaptureRetryMs = 20;

// Tampon stdio fourni par l'application et DÉCLARÉ plus petit qu'il n'est.
// CONFINEMENT d'un bug amont, pas une optimisation : `__bufio_put` de picolibc
// (toolchain ESP-IDF 6.0.1) écrit un octet AU-DELÀ du tampon de sortie, et le
// tampon que fopen() alloue lui-même vit sur le tas — l'octet de trop y écrase
// le canari du bloc voisin et fait paniquer un free() sans rapport, des minutes
// plus tard. Même parade que sd_log::open_file, cf.
// docs/hardware-notes.md (picolibc). Statique (.bss) : aucune
// métadonnée d'allocateur à proximité. Un seul fichier ouvert à la fois ici.
constexpr size_t kFileBufDeclared = 1024;
constexpr size_t kFileBufSlack = 32;
char s_file_buf[kFileBufDeclared + kFileBufSlack];

Hooks s_hooks = {};

// Écrits par la tâche de l'API (on_call_start) et par la tâche LVGL
// (on_answered), lus par la tâche cam_stop et par le timer de non-réponse :
// atomiques, jamais volatile.
std::atomic<bool> s_pending{false};
std::atomic<bool> s_answered{false};

// Lu par la tâche LVGL (pastille de la page météo), écrit par cam_stop.
std::atomic<uint32_t> s_missed{0};

// Prochain numéro de capture. Rotation FIFO : au-delà de kCallHistoryLimit on
// réécrit par-dessus la plus ancienne, la carte ne se remplit donc jamais.
// Touché uniquement par cam_stop (une seule fin d'appel à la fois).
uint32_t s_slot = 0;

// Pas d'atomique, contrairement à s_missed : une chaîne ne se lit pas d'un mot.
// Écrivaine unique : la tâche cam_stop. Lectrice : la tâche LVGL, quand
// l'utilisateur touche la pastille d'appels manqués. Les deux ne peuvent se
// croiser que si un nouvel appel se termine à l'instant précis où l'on ouvre la
// capture du précédent — l'écran caméra étant affiché pendant tout l'appel, la
// pastille n'est alors pas à l'écran. Même raisonnement que prefs::s_ring_file.
char s_last_shot[80] = {};

esp_timer_handle_t s_timer = nullptr;

// La carte peut être absente, en erreur, ou appartenir au PC (mode clé USB) :
// dans ces trois cas on ne touche pas au système de fichiers, mais l'appel
// reste compté comme manqué — le compteur vit en NVS, pas sur la carte.
bool sd_ready() {
  return bsp::Bsp::instance().sd_state() == bsp::SdState::kMounted &&
         usb_msc::status() != usb_msc::Status::kActive;
}

void persist() {
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open %s : historique non persisté", kNamespace);
    return;
  }
  nvs_set_u32(handle, kMissedKey, s_missed.load());
  nvs_set_u32(handle, kSlotKey, s_slot);
  nvs_set_str(handle, kLastKey, s_last_shot);
  nvs_commit(handle);
  nvs_close(handle);
}

// Horodatage local. Sans synchro SNTP l'horloge est à l'epoch : écrire
// "1970-01-01" laisserait croire à une date valide, on marque l'inconnu. Pas de
// '?' dans le motif : "??-" et "??:" sont des trigraphes, que -Wtrigraphs
// signale et que -Werror transformerait en échec de compilation.
void format_now(char *out, size_t cap) {
  const time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  if (t.tm_year < 124) {
    snprintf(out, cap, "0000-00-00 00:00:00");
    return;
  }
  snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900,
           t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}

// Rogne le journal par la TÊTE quand il dépasse kCallLogMaxBytes : une ligne
// par coup de sonnette, sur des années, finirait par remplir la carte alors que
// les captures, elles, rebouclent sur kCallHistoryLimit slots. On garde la
// moitié la plus récente, recoupée sur une frontière de ligne.
void trim_log_if_needed() {
  FILE *f = fopen(cfg::sd::kCallLog, "rb");
  if (f == nullptr) {
    return;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return;
  }
  const long size = ftell(f);
  if (size <= cfg::sd::kCallLogMaxBytes) {
    fclose(f);
    return;
  }
  constexpr long kKeep = cfg::sd::kCallLogMaxBytes / 2;
  char *buf = static_cast<char *>(heap_caps_malloc(kKeep, MALLOC_CAP_SPIRAM));
  if (buf == nullptr || fseek(f, size - kKeep, SEEK_SET) != 0) {
    heap_caps_free(buf);
    fclose(f);
    return;
  }
  const size_t got = fread(buf, 1, (size_t)kKeep, f);
  fclose(f);

  // La lecture démarre au milieu d'une ligne : repartir après le premier '\n',
  // sinon la page Appels afficherait un enregistrement tronqué en tête.
  size_t start = 0;
  while (start < got && buf[start] != '\n') {
    start++;
  }
  if (start < got) {
    start++;
  }

  // Réécriture sous un nom temporaire puis renommage : une coupure de courant
  // en plein milieu laisse l'ancien journal intact plutôt qu'un fichier à
  // moitié écrit.
  char tmp[96];
  snprintf(tmp, sizeof(tmp), "%s.tmp", cfg::sd::kCallLog);
  FILE *o = fopen(tmp, "wb");
  if (o == nullptr) {
    heap_caps_free(buf);
    return;
  }
  setvbuf(o, s_file_buf, _IOFBF, kFileBufDeclared);
  const size_t written = fwrite(buf + start, 1, got - start, o);
  fclose(o);
  heap_caps_free(buf);
  if (written != got - start) {
    unlink(tmp);
    ESP_LOGE(TAG, "rognage du journal : écriture partielle");
    return;
  }
  unlink(cfg::sd::kCallLog); // FAT : rename() échoue si la cible existe
  if (rename(tmp, cfg::sd::kCallLog) != 0) {
    ESP_LOGE(TAG, "rognage du journal : renommage impossible");
    return;
  }
  ESP_LOGI(TAG, "journal d'appels rogné (%ld -> %u o)", size,
           (unsigned)(got - start));
}

void append_log(const char *verdict, const char *path) {
  if (!sd_ready()) {
    return;
  }
  FILE *f = fopen(cfg::sd::kCallLog, "a");
  if (f == nullptr) {
    ESP_LOGE(TAG, "ouverture de %s en ajout impossible", cfg::sd::kCallLog);
    return;
  }
  setvbuf(f, s_file_buf, _IOFBF, kFileBufDeclared);
  // 64 et non 20 : GCC dimensionne sur le PIRE cas de chaque %d (11 octets pour
  // un int), pas sur la plage réelle des champs de struct tm — sous
  // -Werror=format-truncation, un tampon juste assez grand ne compile pas.
  char ts[64];
  format_now(ts, sizeof(ts));
  fprintf(f, "%s,%s,%s\n", ts, verdict, path);
  fclose(f);
  trim_log_if_needed();
}

// Copie la dernière frame de la mailbox MJPEG dans /sdcard/calls/call_NNN.jpg.
// `out` reçoit le chemin écrit. Bloquant (SD) : tâche cam_stop uniquement.
bool save_shot(char *out, size_t cap) {
  out[0] = '\0';
  if (!sd_ready()) {
    return false;
  }
  // Alloué à la demande et rendu tout de suite : 320 Ko immobilisés en
  // permanence pour un usage d'une fois par appel pénaliseraient les Lottie de
  // l'écran météo, qui reprennent la PSRAM dès la fin de session.
  uint8_t *buf =
      static_cast<uint8_t *>(heap_caps_malloc(kShotMax, MALLOC_CAP_SPIRAM));
  if (buf == nullptr) {
    ESP_LOGE(TAG, "alloc capture (%u o) échouée", (unsigned)kShotMax);
    return false;
  }

  size_t len = 0;
  bool got = false;
  for (int i = 0; i < kCaptureTries && !got; i++) {
    got = video_mjpeg::capture_snapshot(buf, kShotMax, &len);
    if (!got) {
      vTaskDelay(pdMS_TO_TICKS(kCaptureRetryMs));
    }
  }
  if (!got) {
    heap_caps_free(buf);
    ESP_LOGW(TAG, "aucune image à archiver (mailbox vide)");
    return false;
  }

  snprintf(out, cap, "%s/call_%03lu.jpg", cfg::sd::kCallDir,
           (unsigned long)s_slot);
  FILE *f = fopen(out, "wb");
  if (f == nullptr) {
    heap_caps_free(buf);
    ESP_LOGE(TAG, "création de %s impossible", out);
    out[0] = '\0';
    return false;
  }
  setvbuf(f, s_file_buf, _IOFBF, kFileBufDeclared);
  const size_t written = fwrite(buf, 1, len, f);
  fclose(f);
  heap_caps_free(buf);
  if (written != len) {
    ESP_LOGE(TAG, "écriture partielle de %s (%u/%u o)", out,
             (unsigned)written, (unsigned)len);
    unlink(out);
    out[0] = '\0';
    return false;
  }
  s_slot = (s_slot + 1) % (uint32_t)cfg::sd::kCallHistoryLimit;
  ESP_LOGI(TAG, "capture %s (%u o)", out, (unsigned)len);
  return true;
}

// Contexte : tâche esp_timer (priorité 22). camera_session::stop() est conçue
// pour rendre la main tout de suite — elle pose deux atomiques et délègue le
// démontage à sa propre tâche — donc pas de tâche intermédiaire à créer ici.
void no_answer_cb(void *) {
  if (!s_pending.load(std::memory_order_relaxed) ||
      s_answered.load(std::memory_order_relaxed)) {
    return;
  }
  if (s_hooks.camera_active == nullptr || !s_hooks.camera_active() ||
      s_hooks.stop_camera == nullptr) {
    return;
  }
  ESP_LOGI(TAG, "personne n'a répondu en %d s : fermeture de l'écran caméra",
           cfg::sd::kCallNoAnswerMs / 1000);
  s_hooks.stop_camera();
}

} // namespace

void init(const Hooks &hooks) {
  s_hooks = hooks;

  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READONLY, &handle) == ESP_OK) {
    uint32_t v = 0;
    if (nvs_get_u32(handle, kMissedKey, &v) == ESP_OK) {
      s_missed.store(v);
    }
    if (nvs_get_u32(handle, kSlotKey, &v) == ESP_OK) {
      s_slot = v % (uint32_t)cfg::sd::kCallHistoryLimit;
    }
    size_t len = sizeof(s_last_shot);
    if (nvs_get_str(handle, kLastKey, s_last_shot, &len) != ESP_OK) {
      // nvs_get_str laisse le tampon indéterminé en cas d'échec.
      s_last_shot[0] = '\0';
    }
    s_last_shot[sizeof(s_last_shot) - 1] = '\0';
    nvs_close(handle);
  }

  if (sd_ready() && mkdir(cfg::sd::kCallDir, 0777) != 0 && errno != EEXIST) {
    ESP_LOGE(TAG, "création de %s impossible (errno %d)", cfg::sd::kCallDir,
             errno);
  }

  esp_timer_create_args_t args = {};
  args.callback = no_answer_cb;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "call_noans";
  if (esp_timer_create(&args, &s_timer) != ESP_OK) {
    // Sans lui, un appel sans réponse laisse l'écran caméra ouvert jusqu'à ce
    // qu'on appuie sur Retour — dégradé, mais pas cassé.
    s_timer = nullptr;
    ESP_LOGE(TAG, "création du timer de non-réponse échouée");
  }

  ESP_LOGI(TAG, "historique : %lu appel(s) manqué(s), prochain slot %lu, "
                "dernière capture %s",
           (unsigned long)s_missed.load(), (unsigned long)s_slot,
           s_last_shot[0] != '\0' ? s_last_shot : "(aucune)");
}

void on_call_start() {
  s_answered.store(false, std::memory_order_relaxed);
  s_pending.store(true, std::memory_order_relaxed);
  if (s_timer == nullptr) {
    return;
  }
  // Ré-armement : esp_timer_start_once refuse un timer déjà armé, et une
  // re-sonnerie doit repartir de zéro. L'échec de stop() sur un timer inactif
  // est le cas nominal du premier appel, pas une erreur.
  esp_timer_stop(s_timer);
  const esp_err_t err = esp_timer_start_once(
      s_timer, (uint64_t)cfg::sd::kCallNoAnswerMs * 1000ULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "armement du timer de non-réponse : %s",
             esp_err_to_name(err));
  }
}

void on_answered() {
  if (!s_pending.load(std::memory_order_relaxed)) {
    return; // accès manuel à la caméra : rien à acquitter
  }
  s_answered.store(true, std::memory_order_relaxed);
  if (s_timer != nullptr) {
    esp_timer_stop(s_timer);
  }
}

bool on_camera_closing() {
  if (s_timer != nullptr) {
    esp_timer_stop(s_timer);
  }
  if (!s_pending.exchange(false, std::memory_order_relaxed)) {
    return false; // pas d'appel en cours : accès manuel, rien à tracer
  }
  if (s_answered.exchange(false, std::memory_order_relaxed)) {
    append_log("REPONDU", "");
    ESP_LOGI(TAG, "appel répondu");
    return false;
  }

  char path[sizeof(s_last_shot)];
  const bool shot = save_shot(path, sizeof(path));
  append_log("MANQUE", path);
  if (shot) {
    snprintf(s_last_shot, sizeof(s_last_shot), "%s", path);
  }
  const uint32_t missed = s_missed.fetch_add(1, std::memory_order_relaxed) + 1;
  persist();
  ESP_LOGI(TAG, "appel manqué (%lu au total)", (unsigned long)missed);
  // true même sans capture : l'utilisateur doit savoir qu'on a sonné. C'est la
  // visionneuse qui signalera le fichier manquant, pas la notification.
  return true;
}

uint32_t missed_count() { return s_missed.load(std::memory_order_relaxed); }

const char *last_shot_path() { return s_last_shot; }

void acknowledge_missed() {
  if (s_missed.exchange(0, std::memory_order_relaxed) == 0) {
    return;
  }
  persist();
}

size_t load_recent(call_log::Entry *out, size_t max) {
  if (out == nullptr || max == 0 || !sd_ready()) {
    return 0;
  }
  FILE *f = fopen(cfg::sd::kCallLog, "rb");
  if (f == nullptr) {
    return 0; // aucun appel encore journalisé : pas une erreur
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 0;
  }
  const long size = ftell(f);
  if (size <= 0) {
    fclose(f);
    return 0;
  }
  // On ne relit que la queue : les entrées affichées sont les plus récentes, et
  // charger un journal de 32 Ko entier en RAM pour n'en garder que 60 lignes
  // n'apporterait rien.
  const long tail =
      size < cfg::sd::kCallLogTailBytes ? size : cfg::sd::kCallLogTailBytes;
  const bool partial = tail < size;
  char *buf = static_cast<char *>(heap_caps_malloc(tail, MALLOC_CAP_SPIRAM));
  if (buf == nullptr || fseek(f, size - tail, SEEK_SET) != 0) {
    heap_caps_free(buf);
    fclose(f);
    ESP_LOGE(TAG, "lecture du journal d'appels impossible");
    return 0;
  }
  const size_t got = fread(buf, 1, (size_t)tail, f);
  fclose(f);

  const size_t n = call_log::parse_tail(buf, got, partial, out, max);
  heap_caps_free(buf);
  return n;
}

} // namespace call_history
