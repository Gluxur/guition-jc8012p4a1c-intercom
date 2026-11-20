#include "ota_ctrl.h"

#include <atomic>
#include <memory>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h" // xTaskCreatePinnedToCoreWithCaps
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sys/mutex.h"
#include "sys/ota_image_check.h"
#include "sys/sd_log.h"

namespace ota_ctrl {
namespace {

constexpr const char *TAG = "ota_ctrl";

bool (*s_is_busy)() = nullptr;
void (*s_report)(const char *) = nullptr;

char s_url[256] = {0};
SemaphoreHandle_t s_req_sem = nullptr; // réveille la tâche OTA
std::atomic<bool> s_in_progress{false};

// Miroir du dernier statut, pour que le catalogue HA puisse le republier. Écrit
// par la tâche `ota`, lu par la tâche de télémétrie : verrou obligatoire, la
// chaîne n'est pas un mot machine. "ready" au boot — c'est ce qui remplace le
// "reboot" laissé par l'OTA précédent, que rien ne venait effacer.
char s_status[48] = "ready";
SemaphoreHandle_t s_status_mtx = nullptr;

// La source est un paramètre de la requête, pas un module à part : dupliquer la
// tâche dédierait deux chemins d'écriture à la même partition, alors que le
// compare_exchange sur s_in_progress n'en protège qu'un. s_source est écrite
// sous cette même garde, donc jamais concurremment.
enum class Source : uint8_t { kHttp, kFile };
Source s_source = Source::kHttp;
char s_path[128] = {0}; // distinct de s_url : un chemin n'est pas une URL

// Lecture par blocs de 8 Ko en PSRAM. La SRAM interne est rare (la pile WebRTC
// en consomme déjà ~71 Ko) et rien ici n'est du DMA.
constexpr size_t kFileChunk = 8192;

struct FileCloser {
  void operator()(FILE *f) const { fclose(f); }
};
struct CapsFree {
  void operator()(void *p) const { heap_caps_free(p); }
};

// Le verrou peut manquer si init() a échoué : sans lui il n'y a de toute façon
// pas de tâche OTA, donc pas de second écrivain. On copie quand même plutôt que
// de laisser l'entité HA sans état.
void store_status(const char *msg) {
  if (s_status_mtx == nullptr) {
    snprintf(s_status, sizeof s_status, "%s", msg);
    return;
  }
  sys::MutexLock lock(s_status_mtx);
  snprintf(s_status, sizeof s_status, "%s", msg);
}

void report(const char *msg) {
  ESP_LOGI(TAG, "%s", msg);
  store_status(msg);
  if (s_report)
    s_report(msg);
}

// Boucle OTA à handle : permet de reporter la progression. L'URL est en https
// (HA ne sert que du TLS sur son port) mais aucun certificat n'est configuré :
// c'est CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY qui rend la connexion possible.
// Attacher le bundle de certificats ne marcherait PAS tant que settings::host
// est une IP : le certificat du serveur porte un nom DNS, et la vérification
// d'identité échouerait.
esp_err_t do_ota(const char *url) {
  esp_http_client_config_t http = {};
  http.url = url;
  http.timeout_ms = 10000;
  http.keep_alive_enable = true;

  esp_https_ota_config_t ota = {};
  ota.http_config = &http;

  esp_https_ota_handle_t handle = nullptr;
  esp_err_t err = esp_https_ota_begin(&ota, &handle);
  if (err != ESP_OK || handle == nullptr) {
    report("error: begin");
    return err != ESP_OK ? err : ESP_FAIL;
  }

  int total = esp_https_ota_get_image_size(handle);
  int last_pct = -1;
  while (true) {
    err = esp_https_ota_perform(handle);
    if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
      break;
    int read = esp_https_ota_get_image_len_read(handle);
    if (total > 0) {
      int pct = (int)((int64_t)read * 100 / total);
      if (pct != last_pct && pct % 5 == 0) { // pas de spam : tous les 5%
        char buf[48];
        snprintf(buf, sizeof(buf), "downloading %d%%", pct);
        report(buf);
        last_pct = pct;
      }
    }
    // Respirer : le download ne doit pas affamer les autres tâches.
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  if (err != ESP_OK) {
    esp_https_ota_abort(handle);
    report("error: perform");
    return err;
  }
  if (!esp_https_ota_is_complete_data_received(handle)) {
    esp_https_ota_abort(handle);
    report("error: incomplete");
    return ESP_FAIL;
  }

  report("verify");
  err = esp_https_ota_finish(handle);
  if (err != ESP_OK) {
    report("error: finish"); // image invalide / signature
    return err;
  }
  return ESP_OK;
}

// OTA depuis un fichier de la carte. Tout ce qui entoure (garde is_busy,
// statut, reboot, remise à zéro de s_in_progress) reste dans ota_task, commun
// aux deux sources.
esp_err_t do_ota_file(const char *path) {
  std::unique_ptr<FILE, FileCloser> f(fopen(path, "rb"));
  if (!f) {
    report("error: open");
    return ESP_ERR_NOT_FOUND;
  }

  struct stat st = {};
  if (stat(path, &st) != 0 || st.st_size <= 0) {
    report("error: stat");
    return ESP_ERR_INVALID_SIZE;
  }

  // Validation AVANT esp_ota_begin, qui EFFACE la partition cible. Sans ce
  // contrôle, un fichier qui n'est pas un firmware détruirait la version
  // précédente — celle-là même vers laquelle le rollback devrait revenir.
  uint8_t head[ota_image::kHeaderProbeBytes];
  if (fread(head, 1, sizeof(head), f.get()) != sizeof(head)) {
    report("error: not a firmware");
    return ESP_ERR_INVALID_SIZE;
  }
  ota_image::Info info = {};
  if (!ota_image::parse_header(head, sizeof(head), &info)) {
    report("error: not a firmware");
    return ESP_ERR_INVALID_ARG;
  }
  ESP_LOGI(TAG, "image SD : %s %s (%s)", info.project, info.version, info.date);

  const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
  if (part == nullptr) {
    report("error: no ota slot");
    return ESP_FAIL;
  }
  if ((size_t)st.st_size > part->size) {
    report("error: too large");
    return ESP_ERR_INVALID_SIZE;
  }

  std::unique_ptr<uint8_t, CapsFree> buf(
      (uint8_t *)heap_caps_malloc(kFileChunk, MALLOC_CAP_SPIRAM));
  if (!buf) {
    report("error: no memory");
    return ESP_ERR_NO_MEM;
  }

  // Taille connue, contrairement au cas HTTP : l'effacement est borné au strict
  // nécessaire au lieu des 6 Mo de la partition.
  esp_ota_handle_t handle = 0;
  esp_err_t err = esp_ota_begin(part, (size_t)st.st_size, &handle);
  if (err != ESP_OK) {
    report("error: begin");
    return err;
  }

  rewind(f.get());
  long written = 0;
  int last_pct = -1;
  for (;;) {
    const size_t n = fread(buf.get(), 1, kFileChunk, f.get());
    if (n == 0) {
      break;
    }
    err = esp_ota_write(handle, buf.get(), n);
    if (err != ESP_OK) {
      esp_ota_abort(handle);
      report("error: write");
      return err;
    }
    written += (long)n;
    const int pct = (int)((int64_t)written * 100 / st.st_size);
    if (pct != last_pct && pct % 5 == 0) { // pas de spam : tous les 5 %
      char msg[48];
      snprintf(msg, sizeof(msg), "downloading %d%%", pct);
      report(msg);
      last_pct = pct;
    }
    // Respirer : l'écriture flash ne doit pas affamer les autres tâches.
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  if (written != (long)st.st_size) {
    esp_ota_abort(handle);
    report("error: incomplete");
    return ESP_FAIL;
  }

  report("verify");
  err = esp_ota_end(handle); // vérifie l'image ; le handle est consommé
  if (err != ESP_OK) {
    report("error: finish");
    return err;
  }
  err = esp_ota_set_boot_partition(part);
  if (err != ESP_OK) {
    report("error: set boot");
    return err;
  }
  return ESP_OK;
}

void ota_task(void *arg) {
  (void)arg;
  for (;;) {
    xSemaphoreTake(s_req_sem, portMAX_DELAY);
    if (s_is_busy && s_is_busy()) {
      report("busy");
      s_in_progress.store(false);
      continue;
    }
    report("start");
    esp_err_t err =
        (s_source == Source::kFile) ? do_ota_file(s_path) : do_ota(s_url);
    if (err == ESP_OK) {
      report("reboot");
      vTaskDelay(pdMS_TO_TICKS(500)); // laisser partir le message MQTT
      sd_log::flush_now();
      esp_restart();
    }
    s_in_progress.store(false);
  }
}

} // namespace

void init(bool (*is_busy)(), void (*report_fn)(const char *msg)) {
  s_is_busy = is_busy;
  s_report = report_fn;
  // Avant la tâche : elle publie un statut dès son premier réveil.
  s_status_mtx = xSemaphoreCreateMutex();
  if (s_status_mtx == nullptr) {
    ESP_LOGE(TAG, "mutex de statut OTA non créé : lecture non protégée");
  }
  s_req_sem = xSemaphoreCreateBinary();
  if (s_req_sem == nullptr) {
    ESP_LOGE(TAG, "création du sémaphore OTA impossible : OTA indisponible");
    return;
  }
  // Pile en PSRAM : le buffer OTA tient large ; SRAM interne rare.
  // 12288 et non 8192 : le chemin fichier ajoute FATFS à la trace d'appel, dont
  // la profondeur a déjà provoqué un dépassement ailleurs dans ce projet (la
  // tâche cam_stop, 4096 o de pile pour 6328 o de pic). Le canari panique au
  // changement de contexte, donc loin de la vraie cause — mieux vaut la marge.
  if (xTaskCreatePinnedToCoreWithCaps(ota_task, "ota", 12288, nullptr, 3,
                                      nullptr, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
    // Sans cette tâche, personne ne consomme le sémaphore : request() laisserait
    // s_in_progress armé pour toujours, ce qui gèle définitivement le watchdog
    // réseau (net_wdt_freeze_cb lit in_progress()).
    ESP_LOGE(TAG, "création de la tâche OTA impossible : OTA indisponible");
    vSemaphoreDelete(s_req_sem);
    s_req_sem = nullptr;
  }
}

void request(const char *url) {
  if (url == nullptr || url[0] == '\0')
    return;
  if (s_req_sem == nullptr) {
    report("error: ota task unavailable");
    return;
  }
  bool expected = false;
  if (!s_in_progress.compare_exchange_strong(expected, true)) {
    report("busy: already in progress");
    return;
  }
  s_source = Source::kHttp;
  strncpy(s_url, url, sizeof(s_url) - 1);
  s_url[sizeof(s_url) - 1] = '\0';
  xSemaphoreGive(s_req_sem);
}

void request_file(const char *path) {
  if (path == nullptr || path[0] == '\0') {
    return;
  }
  if (s_req_sem == nullptr) {
    report("error: ota task unavailable");
    return;
  }
  // Refus et non troncature : un chemin coupé à 127 caractères désigne un
  // AUTRE fichier (le préfixe), que l'OTA aurait flashé si son en-tête était
  // valide. Le serveur de fichiers accepte des chemins plus longs.
  if (strlen(path) >= sizeof(s_path)) {
    report("fail: path too long");
    return;
  }
  bool expected = false;
  if (!s_in_progress.compare_exchange_strong(expected, true)) {
    report("busy: already in progress");
    return;
  }
  s_source = Source::kFile;
  memcpy(s_path, path, strlen(path) + 1);
  xSemaphoreGive(s_req_sem);
}

// Appelée hors de la tâche `ota` (tâche disque de ui_sdlog) : elle ne touche ni
// s_in_progress ni s_path, donc aucune course avec une requête en cours.
void status(char *out, size_t cap) {
  if (out == nullptr || cap == 0) {
    return;
  }
  if (s_status_mtx == nullptr) {
    snprintf(out, cap, "%s", s_status);
    return;
  }
  sys::MutexLock lock(s_status_mtx);
  snprintf(out, cap, "%s", s_status);
}

bool probe_file(const char *path, ImageInfo *out) {
  if (path == nullptr || out == nullptr) {
    return false;
  }
  std::unique_ptr<FILE, FileCloser> f(fopen(path, "rb"));
  if (!f) {
    return false;
  }
  uint8_t head[ota_image::kHeaderProbeBytes];
  if (fread(head, 1, sizeof(head), f.get()) != sizeof(head)) {
    return false;
  }
  ota_image::Info info = {};
  if (!ota_image::parse_header(head, sizeof(head), &info)) {
    return false;
  }
  struct stat st = {};
  snprintf(out->version, sizeof(out->version), "%s", info.version);
  snprintf(out->project, sizeof(out->project), "%s", info.project);
  snprintf(out->date, sizeof(out->date), "%s", info.date);
  out->size_bytes = (stat(path, &st) == 0) ? (uint32_t)st.st_size : 0;
  return true;
}

bool in_progress() { return s_in_progress.load(); }

} // namespace ota_ctrl
