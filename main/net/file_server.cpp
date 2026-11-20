#include "file_server.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "app_config.h"
#include "bsp.h"
#include "net/file_path.h"
#include "sys/mutex.h"

// Page unique servie sur "/" : embarquée dans le binaire par EMBED_FILES. Les
// symboles sont produits par le linker, d'où extern "C" (ABI C) hors namespace.
extern "C" {
extern const uint8_t _binary_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t _binary_index_html_end[] asm("_binary_index_html_end");
}

namespace file_server {
namespace {

constexpr const char *TAG = "file_server";

// 16 Ko : compromis entre le nombre d'allers-retours FATFS et l'occupation
// PSRAM. Aligné 64 par cohérence avec les autres buffers du projet (la ligne
// de cache L2 est à 64 octets, imposée par esp_hosted).
constexpr size_t kXferBytes = 16384;
constexpr size_t kPathMax = 192;

Hooks s_hooks = {};
httpd_handle_t s_server = nullptr;
std::atomic<bool> s_running{false};
uint8_t *s_xfer = nullptr;

struct FileCloser {
  void operator()(FILE *f) const { fclose(f); }
};
struct DirCloser {
  void operator()(DIR *d) const { closedir(d); }
};

// Refus commun. Renvoie true si la requête a été traitée (donc refusée) et que
// l'appelant doit sortir immédiatement.
bool reject_if_busy(httpd_req_t *req) {
  if (s_hooks.sd_busy != nullptr && s_hooks.sd_busy()) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"busy\"}");
    return true;
  }
  return false;
}

// Extrait un paramètre de query string et l'assainit. Répond 400 et renvoie
// false si absent ou refusé.
bool query_path(httpd_req_t *req, const char *key, char *out, size_t out_cap) {
  char query[kPathMax * 2];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"missing_query\"}");
    return false;
  }
  char raw[kPathMax];
  if (httpd_query_key_value(query, key, raw, sizeof(raw)) != ESP_OK) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"missing_param\"}");
    return false;
  }
  if (!file_path::sanitize(raw, out, out_cap)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"bad_path\"}");
    return false;
  }
  return true;
}

esp_err_t h_index(httpd_req_t *req) {
  const size_t len = _binary_index_html_end - _binary_index_html_start;
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, (const char *)_binary_index_html_start, len);
}

esp_err_t h_ls(httpd_req_t *req) {
  if (reject_if_busy(req))
    return ESP_OK;
  char dir[kPathMax];
  if (!query_path(req, "dir", dir, sizeof(dir)))
    return ESP_OK;

  std::unique_ptr<DIR, DirCloser> d(opendir(dir));
  if (!d) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_sendstr(req, "{\"error\":\"no_dir\"}");
    return ESP_OK;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr_chunk(req, "{\"files\":[");
  bool first = true;
  // Dimensionné au PIRE cas et non « à la louche » : dirent::d_name fait 256
  // octets, auxquels s'ajoutent la structure JSON et la taille en décimal.
  // -Werror=format-truncation refuse tout tampon dont il ne peut pas prouver
  // qu'il suffit.
  char item[512];
  for (struct dirent *e = readdir(d.get()); e != nullptr; e = readdir(d.get())) {
    char full[kPathMax];
    // Troncature possible sur un nom très long : on saute l'entrée plutôt que
    // de produire un chemin faux.
    if (snprintf(full, sizeof(full), "%s/%s", dir, e->d_name) >=
        (int)sizeof(full)) {
      continue;
    }
    struct stat st = {};
    const bool is_dir = (stat(full, &st) == 0) && S_ISDIR(st.st_mode);
    snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"size\":%lu,\"dir\":%s}",
             first ? "" : ",", e->d_name, (unsigned long)st.st_size,
             is_dir ? "true" : "false");
    httpd_resp_sendstr_chunk(req, item);
    first = false;
  }
  httpd_resp_sendstr_chunk(req, "]}");
  return httpd_resp_sendstr_chunk(req, nullptr);
}

esp_err_t h_dl(httpd_req_t *req) {
  if (reject_if_busy(req))
    return ESP_OK;
  char path[kPathMax];
  if (!query_path(req, "path", path, sizeof(path)))
    return ESP_OK;

  std::unique_ptr<FILE, FileCloser> f(fopen(path, "rb"));
  if (!f) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_sendstr(req, "{\"error\":\"no_file\"}");
    return ESP_OK;
  }
  httpd_resp_set_type(req, "application/octet-stream");
  const char *name = strrchr(path, '/');
  char disp[kPathMax + 48];
  snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"",
           name ? name + 1 : "fichier");
  httpd_resp_set_hdr(req, "Content-Disposition", disp);

  for (;;) {
    const size_t n = fread(s_xfer, 1, kXferBytes, f.get());
    if (n == 0)
      break;
    if (httpd_resp_send_chunk(req, (const char *)s_xfer, n) != ESP_OK) {
      // Client parti : la socket est morte, pas de chunk terminal à envoyer.
      // Le unique_ptr ferme le fichier.
      return ESP_FAIL;
    }
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t h_up(httpd_req_t *req) {
  if (reject_if_busy(req))
    return ESP_OK;
  char path[kPathMax];
  if (!query_path(req, "path", path, sizeof(path)))
    return ESP_OK;

  std::unique_ptr<FILE, FileCloser> f(fopen(path, "wb"));
  if (!f) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\":\"open_failed\"}");
    return ESP_OK;
  }

  int remaining = req->content_len;
  while (remaining > 0) {
    const int want = remaining < (int)kXferBytes ? remaining : (int)kXferBytes;
    const int got = httpd_req_recv(req, (char *)s_xfer, want);
    if (got <= 0) {
      f.reset();
      unlink(path); // un fichier tronqué qui porte le bon nom est pire que rien
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_sendstr(req, "{\"error\":\"recv_failed\"}");
      return ESP_OK;
    }
    if (fwrite(s_xfer, 1, got, f.get()) != (size_t)got) {
      f.reset();
      unlink(path);
      httpd_resp_set_status(req, "507 Insufficient Storage");
      httpd_resp_sendstr(req, "{\"error\":\"write_failed\"}");
      return ESP_OK;
    }
    remaining -= got;
  }
  f.reset(); // fermeture explicite AVANT de répondre : la réponse vaut promesse
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t h_rm(httpd_req_t *req) {
  if (reject_if_busy(req))
    return ESP_OK;
  char path[kPathMax];
  if (!query_path(req, "path", path, sizeof(path)))
    return ESP_OK;
  if (unlink(path) != 0) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_sendstr(req, "{\"error\":\"unlink_failed\"}");
    return ESP_OK;
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t h_info(httpd_req_t *req) {
  uint64_t total = 0, freeb = 0;
  const bool busy = s_hooks.sd_busy != nullptr && s_hooks.sd_busy();
  // sd_capacity() lit la carte : on ne l'appelle pas quand elle est occupée.
  const bool ok = !busy && bsp::Bsp::instance().sd_capacity(&total, &freeb);
  char buf[128];
  snprintf(buf, sizeof(buf),
           "{\"total\":%llu,\"free\":%llu,\"busy\":%s,\"ok\":%s}",
           (unsigned long long)total, (unsigned long long)freeb,
           busy ? "true" : "false", ok ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, buf);
}

esp_err_t h_ota(httpd_req_t *req) {
  if (s_hooks.flash_firmware == nullptr) {
    httpd_resp_set_status(req, "501 Not Implemented");
    httpd_resp_sendstr(req, "{\"error\":\"no_ota\"}");
    return ESP_OK;
  }
  if (reject_if_busy(req))
    return ESP_OK;
  char path[kPathMax];
  if (!query_path(req, "path", path, sizeof(path)))
    return ESP_OK;
  // Réponse AVANT de déclencher : l'appareil redémarrera à la fin du flash, le
  // navigateur doit avoir sa réponse d'ici là.
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");
  s_hooks.flash_firmware(path);
  return ESP_OK;
}

// Initialisation DÉSIGNÉE et non positionnelle : les champs websocket de
// httpd_uri_t n'existent que si CONFIG_HTTPD_WS_SUPPORT est armé (il ne l'est
// pas ici). Une liste positionnelle plus longue que la struct effective ne
// compile pas, et changerait de sens si l'option était activée un jour.
const httpd_uri_t kUris[] = {
    {.uri = "/", .method = HTTP_GET, .handler = h_index, .user_ctx = nullptr},
    {.uri = "/api/ls", .method = HTTP_GET, .handler = h_ls, .user_ctx = nullptr},
    {.uri = "/api/dl", .method = HTTP_GET, .handler = h_dl, .user_ctx = nullptr},
    {.uri = "/api/info", .method = HTTP_GET, .handler = h_info,
     .user_ctx = nullptr},
    {.uri = "/api/up", .method = HTTP_POST, .handler = h_up,
     .user_ctx = nullptr},
    {.uri = "/api/rm", .method = HTTP_POST, .handler = h_rm,
     .user_ctx = nullptr},
    {.uri = "/api/ota", .method = HTTP_POST, .handler = h_ota,
     .user_ctx = nullptr},
};

} // namespace

// Sérialise start/stop : appelés depuis esp-mqtt (entité HA) ET depuis la
// tâche LVGL (interrupteur de la page Configuration). Le test-puis-action sur
// s_running permettait un double httpd_start, ou un stop() libérant s_xfer
// pendant un start() en cours.
static SemaphoreHandle_t s_mux = nullptr;

void init(const Hooks &hooks) {
  s_hooks = hooks;
  s_mux = xSemaphoreCreateMutex();
}

static bool start_locked();
static void stop_locked();

bool start() {
  if (s_mux == nullptr) {
    return start_locked();
  }
  sys::MutexLock lock(s_mux);
  return start_locked();
}

void stop() {
  if (s_mux == nullptr) {
    stop_locked();
    return;
  }
  sys::MutexLock lock(s_mux);
  stop_locked();
}

static bool start_locked() {
  if (s_running.load()) {
    return true;
  }
  s_xfer = (uint8_t *)heap_caps_aligned_alloc(64, kXferBytes, MALLOC_CAP_SPIRAM);
  if (s_xfer == nullptr) {
    ESP_LOGE(TAG, "tampon de transfert (%u o PSRAM) indisponible",
             (unsigned)kXferBytes);
    return false;
  }

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  // Cœur 0, priorité 3 : sous la tâche `mjpeg` (drain TCP), qui est le facteur
  // limitant du débit vidéo. Le serveur refuse de toute façon toute IO pendant
  // une session, mais l'ordonnancement doit rester explicite.
  cfg.core_id = 0;
  cfg.task_priority = 3;
  cfg.stack_size = 8192; // SRAM interne : esp_http_server n'offre pas la PSRAM
  cfg.max_open_sockets = 4;
  cfg.max_uri_handlers = 8;
  cfg.lru_purge_enable = true;
  cfg.recv_wait_timeout = 10;
  cfg.send_wait_timeout = 10;

  esp_err_t err = httpd_start(&s_server, &cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start -> %s", esp_err_to_name(err));
    heap_caps_free(s_xfer);
    s_xfer = nullptr;
    s_server = nullptr;
    return false;
  }
  for (const httpd_uri_t &u : kUris) {
    err = httpd_register_uri_handler(s_server, &u);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "register %s -> %s", u.uri, esp_err_to_name(err));
      httpd_stop(s_server);
      s_server = nullptr;
      heap_caps_free(s_xfer);
      s_xfer = nullptr;
      return false;
    }
  }
  s_running.store(true);
  ESP_LOGI(TAG, "serveur de fichiers démarré (port %d)", cfg.server_port);
  return true;
}

static void stop_locked() {
  if (!s_running.load()) {
    return;
  }
  s_running.store(false);
  // httpd_stop attend la fin des requêtes en cours : aucun handler ne peut
  // encore lire s_xfer quand on le libère.
  httpd_stop(s_server);
  s_server = nullptr;
  heap_caps_free(s_xfer);
  s_xfer = nullptr;
  ESP_LOGI(TAG, "serveur de fichiers arrêté");
}

bool running() { return s_running.load(); }

} // namespace file_server
