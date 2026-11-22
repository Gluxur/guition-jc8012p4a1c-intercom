#include "ota_ctrl.h"

#include <atomic>
#include <memory>
#include <string.h>
#include <sys/stat.h>
#include <type_traits>

#include "app_config.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
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
void (*s_changed)() = nullptr;

// Vérification de version : faite par la tâche `ota` elle-même, jamais en
// parallèle d'un téléchargement — deux sessions TLS simultanées doubleraient
// la SRAM interne réclamée par mbedTLS.
std::atomic<bool> s_check_pending{false};
// Armé au premier request_check() : la vérification périodique n'a de sens
// qu'une fois le Wi-Fi et HA là.
std::atomic<bool> s_check_enabled{false};
// Réponse de l'API GitHub : une release avec ses notes tient en quelques Ko.
constexpr size_t kApiBufBytes = 32 * 1024;
// En secondes × configTICK_RATE_HZ, pas pdMS_TO_TICKS : ce dernier multiplie
// en TickType_t 32 bits, et 21,6 M ms × 1000 Hz déborde.
constexpr TickType_t kCheckPeriod = (TickType_t)(6 * 3600) * configTICK_RATE_HZ;
// Sous s_status_mtx, comme le statut : c'est le même lecteur (tâche API).
github_release::Release s_latest = {};
bool s_latest_known = false;
char s_api_url[160] = {};
std::atomic<int> s_progress{-1};

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

void notify_changed() {
  if (s_changed != nullptr) {
    s_changed();
  }
}

void store_latest(const github_release::Release &rel) {
  if (s_status_mtx == nullptr) {
    s_latest = rel;
    s_latest_known = true;
    return;
  }
  sys::MutexLock lock(s_status_mtx);
  s_latest = rel;
  s_latest_known = true;
}

struct HttpCloser {
  void operator()(esp_http_client_handle_t c) const {
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
  }
};

// GET /repos/<repo>/releases/latest, certificat vérifié (bundle de l'IDF).
// Renvoie false sur erreur réseau ou réponse inattendue : le dernier résultat
// connu est alors gardé.
bool do_check() {
  esp_http_client_config_t http = {};
  http.url = s_api_url;
  http.timeout_ms = 10000;
  http.crt_bundle_attach = esp_crt_bundle_attach;
  http.user_agent = cfg::kHttpUserAgent;
  std::unique_ptr<std::remove_pointer_t<esp_http_client_handle_t>, HttpCloser>
      client(esp_http_client_init(&http));
  if (!client) {
    return false;
  }
  esp_http_client_set_header(client.get(), "Accept", "application/vnd.github+json");
  if (esp_http_client_open(client.get(), 0) != ESP_OK) {
    ESP_LOGW(TAG, "check: connexion à GitHub impossible");
    return false;
  }
  esp_http_client_fetch_headers(client.get());
  const int code = esp_http_client_get_status_code(client.get());
  if (code == 404) {
    // Aucune release publiée, ou dépôt privé : rien à proposer.
    ESP_LOGI(TAG, "check: aucune release (HTTP 404)");
    store_latest(github_release::Release{});
    return true;
  }
  if (code != 200) {
    // 403 = quota de l'API anonyme (60/h) : on réessaiera au prochain tour.
    ESP_LOGW(TAG, "check: HTTP %d", code);
    return false;
  }
  std::unique_ptr<char, CapsFree> buf(
      static_cast<char *>(heap_caps_malloc(kApiBufBytes, MALLOC_CAP_SPIRAM)));
  if (!buf) {
    return false;
  }
  size_t got = 0;
  for (;;) {
    const int n = esp_http_client_read(client.get(), buf.get() + got,
                                       (int)(kApiBufBytes - 1 - got));
    if (n <= 0) {
      break;
    }
    got += (size_t)n;
    if (got >= kApiBufBytes - 1) {
      ESP_LOGW(TAG, "check: réponse > %u o, ignorée", (unsigned)kApiBufBytes);
      return false;
    }
  }
  buf.get()[got] = '\0';
  github_release::Release rel = {};
  if (!github_release::parse_latest(buf.get(), cfg::kGithubRepo,
                                    cfg::kGithubAsset, &rel)) {
    ESP_LOGW(TAG, "check: release sans asset %s valide", cfg::kGithubAsset);
    store_latest(github_release::Release{});
    return true;
  }
  ESP_LOGI(TAG, "check: dernière release %s", rel.tag);
  store_latest(rel);
  return true;
}

// Boucle OTA à handle : permet de reporter la progression. Certificat vérifié
// (bundle de l'IDF) : la seule source HTTP est désormais un asset de release
// GitHub, qui redirige vers son stockage — esp_https_ota suit la redirection.
esp_err_t do_ota(const char *url) {
  esp_http_client_config_t http = {};
  http.url = url;
  http.timeout_ms = 10000;
  http.keep_alive_enable = true;
  http.crt_bundle_attach = esp_crt_bundle_attach;
  http.user_agent = cfg::kHttpUserAgent;
  // L'URL de l'asset GitHub et sa redirection signée dépassent le tampon
  // d'en-têtes par défaut (512 o).
  http.buffer_size = 2048;
  http.buffer_size_tx = 2048;

  esp_https_ota_config_t ota = {};
  ota.http_config = &http;

  esp_https_ota_handle_t handle = nullptr;
  esp_err_t err = esp_https_ota_begin(&ota, &handle);
  if (err != ESP_OK || handle == nullptr) {
    report("error: begin");
    return err != ESP_OK ? err : ESP_FAIL;
  }

  // Avant d'écrire quoi que ce soit : refuser une image d'un autre projet.
  esp_app_desc_t incoming = {};
  const esp_app_desc_t *self = esp_app_get_description();
  if (esp_https_ota_get_img_desc(handle, &incoming) != ESP_OK ||
      (self != nullptr &&
       strncmp(incoming.project_name, self->project_name,
               sizeof incoming.project_name) != 0)) {
    esp_https_ota_abort(handle);
    report("error: other project");
    return ESP_FAIL;
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
      s_progress.store(pct);
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
    s_progress.store(pct);
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
    // Le délai d'attente EST la vérification périodique : pas de timer de plus.
    if (xSemaphoreTake(s_req_sem, kCheckPeriod) != pdTRUE &&
        s_check_enabled.load()) {
      s_check_pending.store(true);
    }
    if (s_check_pending.exchange(false)) {
      if (!(s_is_busy && s_is_busy()) && do_check()) {
        notify_changed();
      }
    }
    // s_in_progress armé = une requête attend : seule cette tâche le retombe.
    if (!s_in_progress.load()) {
      continue;
    }
    if (s_is_busy && s_is_busy()) {
      report("busy");
      s_in_progress.store(false);
      notify_changed();
      continue;
    }
    report("start");
    esp_err_t err =
        (s_source == Source::kFile) ? do_ota_file(s_path) : do_ota(s_url);
    if (err == ESP_OK) {
      report("reboot");
      vTaskDelay(pdMS_TO_TICKS(500)); // laisser partir le statut vers HA
      sd_log::flush_now();
      esp_restart();
    }
    s_progress.store(-1);
    s_in_progress.store(false);
    // Après le store : un abonné qui relit in_progress() doit voir false.
    notify_changed();
  }
}

} // namespace

void init(bool (*is_busy)(), void (*report_fn)(const char *msg),
          void (*changed)()) {
  s_is_busy = is_busy;
  s_report = report_fn;
  s_changed = changed;
  // Avant la tâche : elle publie un statut dès son premier réveil.
  snprintf(s_api_url, sizeof s_api_url,
           "https://api.github.com/repos/%s/releases/latest", cfg::kGithubRepo);
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

int progress() { return s_in_progress.load() ? s_progress.load() : -1; }

void request_check() {
  if (s_req_sem == nullptr) {
    return;
  }
  s_check_enabled.store(true);
  s_check_pending.store(true);
  xSemaphoreGive(s_req_sem);
}

bool request_latest() {
  github_release::Release rel = {};
  if (!latest(&rel) || rel.tag[0] == '\0') {
    report("fail: no release known");
    return false;
  }
  if (s_in_progress.load()) {
    report("busy: already in progress");
    return false;
  }
  request(rel.asset_url);
  return true;
}

bool latest(github_release::Release *out) {
  if (out == nullptr) {
    return false;
  }
  if (s_status_mtx == nullptr) {
    *out = s_latest;
    return s_latest_known;
  }
  sys::MutexLock lock(s_status_mtx);
  *out = s_latest;
  return s_latest_known;
}

} // namespace ota_ctrl
