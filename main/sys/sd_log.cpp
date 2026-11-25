#include "sd_log.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app_config.h"
#include "bsp.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sys/crumb.h"
#include "sys/dbg_log.h"
#include "sys/prefs.h"

namespace sd_log {

namespace {

constexpr char TAG[] = "sdlog";

char *s_ring = nullptr;
size_t s_head = 0;  // position d'écriture
size_t s_count = 0; // octets disponibles à la lecture
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

std::atomic<uint32_t> s_dropped{0};
std::atomic<bool> s_paused{false};
// Acquittement de la suspension par la tâche d'écriture : passe à true une fois
// son fichier FERMÉ, donc quand plus aucune I/O n'est en vol. C'est ce que
// pause_and_wait() attend avant de laisser formater la carte.
std::atomic<bool> s_paused_ack{false};
std::atomic<Health> s_health{Health::kDisabled};
std::atomic<bool> s_flush_request{false};
// Interrupteur utilisateur. Vrai par défaut : les lignes du tout début de boot
// entrent dans le ring avant même que prefs soit chargée, start() tranche.
std::atomic<bool> s_enabled{true};

// Touchés uniquement par la tâche d'écriture (s_file_name est lu par l'UI,
// mais c'est une chaîne qui ne change qu'au changement de jour : une lecture
// concurrente donne au pire l'ancien nom pendant un rafraîchissement).
FILE *s_file = nullptr;

// Tampon stdio du fichier de journal, fourni par l'application (cf. le
// commentaire de open_file). Déclaré 1024 à setvbuf alors qu'il en fait 1056 :
// les 32 octets de marge absorbent le débordement d'un octet de picolibc.
constexpr size_t kFileBufDeclared = 1024;
constexpr size_t kFileBufSlack = 32;
char s_file_buf[kFileBufDeclared + kFileBufSlack];
char s_file_name[32] = {};
int s_consecutive_errors = 0;
bool s_error_reported = false;
int64_t s_start_us = 0;

// Le sink s'exécute dans le contexte de l'appelant du log. Aucun accès
// fichier ici : memcpy sous spinlock, rien d'autre. Deux memcpy avec au plus
// un repli, comme dans dbg_log — surtout pas de modulo par octet, ce bloc
// tourne interruptions coupées.
void line_sink(const char *line, size_t len) {
  if (!s_ring || len == 0 || !s_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  if (len > static_cast<size_t>(cfg::sd::kLineMax)) {
    len = static_cast<size_t>(cfg::sd::kLineMax);
  }
  const size_t cap = static_cast<size_t>(cfg::sd::kRingBytes);

  portENTER_CRITICAL(&s_mux);
  size_t first = cap - s_head;
  if (first > len) {
    first = len;
  }
  memcpy(s_ring + s_head, line, first);
  if (len > first) {
    memcpy(s_ring, line + first, len - first);
  }
  s_head = (s_head + len) % cap;
  s_count += len;
  bool overflow = false;
  if (s_count > cap) {
    s_count = cap; // ring plein : les octets les plus anciens sont perdus
    overflow = true;
  }
  portEXIT_CRITICAL(&s_mux);

  if (overflow) {
    s_dropped.fetch_add(1);
  }
}

// Retire jusqu'à `cap` octets, les plus ANCIENS d'abord — contrairement à
// dbg_log::drain qui privilégie les plus récents : ici on écrit un journal
// séquentiel, l'ordre chronologique prime.
// Taille d'une passe de copie sous spinlock. Le drain demande jusqu'à
// kWriteChunkBytes (4 Ko) d'un coup : copier tout ça INTERRUPTIONS COUPÉES
// coûtait des dizaines de microsecondes de latence d'interruption, sur le cœur
// 0 — celui du drain TCP MJPEG et des interruptions SDIO/I2S. C'est l'ordre de
// grandeur que dbg_log.cpp a explicitement chassé pour bien moins que ça.
//
// Sortir le memcpy du verrou ne marche PAS ici : line_sink a plusieurs
// producteurs et écrase les octets les plus anciens sans condition quand le
// ring est plein. Décrémenter s_count puis copier hors verrou libère justement
// la zone où le producteur suivant va écrire — lecture déchirée sous forte
// charge de logs, exactement quand ça compte. On garde donc la copie sous
// verrou, mais par tranches : 16 sections critiques courtes au lieu d'une
// longue, avec une fenêtre d'interruption entre chacune.
constexpr size_t kTakeChunk = 256;

size_t ring_take(char *dst, size_t cap) {
  const size_t ring_cap = static_cast<size_t>(cfg::sd::kRingBytes);
  size_t taken = 0;
  while (taken < cap) {
    portENTER_CRITICAL(&s_mux);
    size_t n = cap - taken;
    if (n > s_count) {
      n = s_count;
    }
    if (n > kTakeChunk) {
      n = kTakeChunk;
    }
    if (n == 0) {
      portEXIT_CRITICAL(&s_mux); // ring vide : rien de plus à prendre
      break;
    }
    const size_t tail = (s_head + ring_cap - s_count) % ring_cap;
    size_t first = ring_cap - tail;
    if (first > n) {
      first = n;
    }
    memcpy(dst + taken, s_ring + tail, first);
    if (n > first) {
      memcpy(dst + taken + first, s_ring, n - first);
    }
    s_count -= n;
    portEXIT_CRITICAL(&s_mux);
    taken += n;
  }
  return taken;
}

// Jette ce qui reste dans le ring. Appelé à l'extinction du journal : sans ça
// les lignes accumulées avant la coupure resteraient en attente d'une écriture
// qui ne viendra pas, et le prochain rallumage les daterait de l'heure courante.
void ring_discard() {
  portENTER_CRITICAL(&s_mux);
  s_count = 0;
  portEXIT_CRITICAL(&s_mux);
}

size_t ring_pending() {
  portENTER_CRITICAL(&s_mux);
  const size_t n = s_count;
  portEXIT_CRITICAL(&s_mux);
  return n;
}

// true dès que SNTP a posé une heure crédible : l'epoch par défaut est 1970.
bool time_is_valid() { return time(nullptr) > 1700000000; /* > 2023-11-14 */ }

const char *reset_reason_str() {
  switch (esp_reset_reason()) {
  case ESP_RST_POWERON:
    return "POWERON";
  case ESP_RST_EXT:
    return "EXT";
  case ESP_RST_SW:
    return "SW";
  case ESP_RST_PANIC:
    return "PANIC";
  case ESP_RST_INT_WDT:
    return "INT_WDT";
  case ESP_RST_TASK_WDT:
    return "TASK_WDT";
  case ESP_RST_WDT:
    return "WDT";
  case ESP_RST_DEEPSLEEP:
    return "DEEPSLEEP";
  case ESP_RST_BROWNOUT:
    return "BROWNOUT";
  case ESP_RST_SDIO:
    return "SDIO";
  case ESP_RST_USB:
    return "USB";
  case ESP_RST_JTAG:
    return "JTAG";
  case ESP_RST_EFUSE:
    return "EFUSE";
  case ESP_RST_PWR_GLITCH:
    return "PWR_GLITCH";
  case ESP_RST_CPU_LOCKUP:
    return "CPU_LOCKUP";
  default:
    return "UNKNOWN";
  }
}

// "AAAA-MM-JJ.txt" (heure locale) ou "nodate.txt" tant que SNTP n'a rien posé.
void target_file_name(char *out, size_t cap) {
  if (!time_is_valid()) {
    snprintf(out, cap, "nodate.txt");
    return;
  }
  const time_t now = time(nullptr);
  struct tm tm_now = {};
  localtime_r(&now, &tm_now);
  snprintf(out, cap, "%04d-%02d-%02d.txt", tm_now.tm_year + 1900,
           tm_now.tm_mon + 1, tm_now.tm_mday);
}

bool open_file(const char *name) {
  char path[96];
  snprintf(path, sizeof(path), "%s/%s", cfg::sd::kLogDir, name);
  FILE *f = fopen(path, "a");
  if (!f) {
    return false;
  }
  // CONFINEMENT d'un bug amont, pas une optimisation. Ne pas retirer sans lire
  // docs/hardware-notes.md (picolibc).
  //
  // `__bufio_put` de picolibc (bufio.c:183, toolchain ESP-IDF 6.0.1) écrit UN
  // octet AU-DELÀ du tampon de sortie. Le tampon que fopen() alloue lui-même
  // fait BUFSIZ = 128 octets et vit SUR LE TAS : l'octet de trop écrase le
  // canari de queue du bloc voisin, et la panique tombe des minutes plus tard
  // dans un free() sans aucun rapport.
  //
  // En fournissant le tampon nous-mêmes et en le déclarant PLUS PETIT qu'il
  // n'est, le débordement tombe dans une marge qui nous appartient. Le tampon
  // est statique (.bss), donc sans métadonnée d'allocateur à proximité.
  // Un seul fichier de journal est ouvert à la fois : un tampon unique suffit.
  setvbuf(f, s_file_buf, _IOFBF, kFileBufDeclared);
  s_file = f;
  snprintf(s_file_name, sizeof(s_file_name), "%s", name);
  return true;
}

void close_file() {
  if (s_file) {
    fflush(s_file);
    fsync(fileno(s_file));
    fclose(s_file);
    s_file = nullptr;
  }
}

void write_boot_banner() {
  if (!s_file) {
    return;
  }
  char stamp[24] = "(heure inconnue)";
  if (time_is_valid()) {
    const time_t now = time(nullptr);
    struct tm tm_now = {};
    localtime_r(&now, &tm_now);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_now);
  }
  const esp_app_desc_t *desc = esp_app_get_description();
  fprintf(s_file,
          "\n==== BOOT #%lu  %s  reset=%s  app=%s  heap=%u  psram=%u ====\n",
          static_cast<unsigned long>(prefs::boot_count()), stamp,
          reset_reason_str(), desc ? desc->version : "?",
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

// Les fichiers portent leur date en nom : aucun stat n'est nécessaire pour
// les trier ni pour décider de leur suppression.
bool is_dated_log(const char *name) {
  return strlen(name) == 14 && strcmp(name + 10, ".txt") == 0;
}

void purge_old_files() {
  if (!time_is_valid()) {
    return; // sans date de référence, ne rien supprimer
  }
  const time_t cutoff =
      time(nullptr) - static_cast<time_t>(cfg::sd::kRetentionDays) * 24 * 3600;
  struct tm tm_cut = {};
  localtime_r(&cutoff, &tm_cut);
  // Dimensionné au pire cas de %04d-%02d-%02d (les %d ne sont pas bornés par
  // la largeur demandée) : sinon -Werror=format-truncation.
  char cut_name[32];
  snprintf(cut_name, sizeof(cut_name), "%04d-%02d-%02d", tm_cut.tm_year + 1900,
           tm_cut.tm_mon + 1, tm_cut.tm_mday);

  DIR *dir = opendir(cfg::sd::kLogDir);
  if (!dir) {
    return;
  }
  struct dirent *ent = nullptr;
  while ((ent = readdir(dir)) != nullptr) {
    // nodate.txt et tout fichier étranger sont laissés intacts.
    if (!is_dated_log(ent->d_name)) {
      continue;
    }
    if (strncmp(ent->d_name, cut_name, 10) < 0) {
      char path[96];
      snprintf(path, sizeof(path), "%s/%s", cfg::sd::kLogDir, ent->d_name);
      unlink(path);
    }
  }
  closedir(dir);
}

// Supprime le plus ancien fichier daté (hors fichier courant). false s'il n'y
// a plus rien à supprimer.
bool free_one_oldest() {
  DIR *dir = opendir(cfg::sd::kLogDir);
  if (!dir) {
    return false;
  }
  char oldest[32] = {};
  struct dirent *ent = nullptr;
  while ((ent = readdir(dir)) != nullptr) {
    if (!is_dated_log(ent->d_name)) {
      continue;
    }
    if (s_file_name[0] && strcmp(ent->d_name, s_file_name) == 0) {
      continue;
    }
    if (oldest[0] == '\0' || strcmp(ent->d_name, oldest) < 0) {
      snprintf(oldest, sizeof(oldest), "%s", ent->d_name);
    }
  }
  closedir(dir);
  if (oldest[0] == '\0') {
    return false;
  }
  char path[96];
  snprintf(path, sizeof(path), "%s/%s", cfg::sd::kLogDir, oldest);
  return unlink(path) == 0;
}

void enforce_free_space() {
  uint64_t total = 0, freeb = 0;
  // Borne dure sur le nombre de suppressions : une carte pleine d'autre chose
  // ne doit pas transformer cette boucle en balayage sans fin.
  for (int guard = 0; guard < 32; guard++) {
    if (!bsp::Bsp::instance().sd_capacity(&total, &freeb)) {
      return;
    }
    if (freeb >= static_cast<uint64_t>(cfg::sd::kMinFreeMb) * 1024 * 1024) {
      return;
    }
    if (!free_one_oldest()) {
      return;
    }
  }
}

// Préfixe chaque ligne de l'heure murale : les horodatages d'esp_log sont en
// millisecondes depuis le boot, inexploitables après un redémarrage.
bool write_chunk_impl(const char *buf, size_t n);

// L'écriture SD partage le contrôleur SDMMC avec la liaison Wi-Fi (SDIO) :
// marqueur autour de chaque bloc pour voir si un blocage tombe dedans.
bool write_chunk(const char *buf, size_t n) {
  crumb::mark(crumb::Id::kSdLogWriteBegin, static_cast<uint32_t>(n));
  const bool ok = write_chunk_impl(buf, n);
  crumb::mark(crumb::Id::kSdLogWriteEnd, ok ? 1 : 0);
  return ok;
}

bool write_chunk_impl(const char *buf, size_t n) {
  char stamp[12] = {};
  if (time_is_valid()) {
    const time_t now = time(nullptr);
    struct tm tm_now = {};
    localtime_r(&now, &tm_now);
    strftime(stamp, sizeof(stamp), "%H:%M:%S ", &tm_now);
  }
  // Le drapeau persiste d'un bloc à l'autre : une ligne coupée par la
  // frontière d'un bloc de 4 Ko ne doit pas recevoir un horodatage en plein
  // milieu.
  static bool at_line_start = true;
  size_t start = 0;
  while (start < n) {
    const char *nl =
        static_cast<const char *>(memchr(buf + start, '\n', n - start));
    const size_t end = nl ? static_cast<size_t>(nl - buf) + 1 : n;
    if (at_line_start && stamp[0] && fputs(stamp, s_file) == EOF) {
      return false;
    }
    if (fwrite(buf + start, 1, end - start, s_file) != end - start) {
      return false;
    }
    at_line_start = (nl != nullptr);
    start = end;
  }
  return true;
}

void writer_task(void *arg) {
  (void)arg;
  // Trop gros pour la pile de la tâche (4 Ko) : statique, tâche unique donc
  // non réentrante.
  static char chunk[cfg::sd::kWriteChunkBytes];

  s_start_us = esp_timer_get_time();
  bool banner_written = false;
  int64_t last_flush_us = 0;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(200));

    if (s_paused.load()) {
      // Fermer le fichier AVANT d'acquitter : c'est la fermeture qui garantit
      // qu'aucun descripteur ne reste ouvert sur la partition que l'appelant
      // s'apprête à formater. Le ring continue d'accumuler pendant ce temps.
      close_file();
      s_paused_ack.store(true);
      continue;
    }
    if (!s_enabled.load()) {
      if (s_file) {
        // Vidange finale avant fermeture : la ligne « journal désactivé » posée
        // par set_enabled() est déjà dans le ring, et c'est la seule chose qui
        // expliquera le silence à la relecture du fichier.
        size_t n = 0;
        while ((n = ring_take(chunk, sizeof(chunk))) > 0) {
          if (!write_chunk(chunk, n)) {
            break;
          }
        }
        close_file();
      }
      ring_discard();
      // Même acquittement que la pause : un formatage ou un passage en mode clé
      // USB demandé pendant que le journal est éteint doit voir le fichier
      // fermé, pas seulement supposer qu'il l'est. L'état kDisabled n'est posé
      // qu'APRÈS close_file(), sinon pause_and_wait court-circuiterait son
      // attente alors qu'un descripteur est encore ouvert.
      s_paused_ack.store(true);
      s_health.store(Health::kDisabled);
      continue;
    }
    s_paused_ack.store(false);
    if (bsp::Bsp::instance().sd_state() != bsp::SdState::kMounted) {
      s_health.store(Health::kError);
      continue;
    }

    // Ouverture différée : tant que l'heure est inconnue on garde tout en
    // RAM, pour que les lignes du boot atterrissent dans le bon fichier daté.
    if (!s_file) {
      const bool timed_out =
          (esp_timer_get_time() - s_start_us) >
          static_cast<int64_t>(cfg::sd::kTimeWaitMs) * 1000;
      if (!time_is_valid() && !timed_out) {
        s_health.store(Health::kBuffering);
        continue;
      }
      char name[32];
      target_file_name(name, sizeof(name));
      if (!open_file(name)) {
        s_health.store(Health::kError);
        continue;
      }
      purge_old_files();
      enforce_free_space();
      if (!banner_written) {
        write_boot_banner();
        banner_written = true;
      }
      s_health.store(Health::kWriting);
    }

    // Bascule de jour, ou passage de nodate.txt au fichier daté.
    char want[32];
    target_file_name(want, sizeof(want));
    if (strcmp(want, s_file_name) != 0) {
      fprintf(s_file, "---- suite dans %s ----\n", want);
      char previous[32];
      snprintf(previous, sizeof(previous), "%s", s_file_name);
      close_file();
      if (!open_file(want)) {
        s_health.store(Health::kError);
        continue;
      }
      fprintf(s_file, "---- suite de %s ----\n", previous);
      purge_old_files();
      enforce_free_space();
    }

    const size_t pending = ring_pending();
    const bool forced = s_flush_request.exchange(false);
    if (pending == 0) {
      if (forced) {
        fflush(s_file);
        fsync(fileno(s_file));
      }
      continue;
    }
    if (!forced && pending < static_cast<size_t>(cfg::sd::kFlushThresholdBytes)) {
      const int64_t now_us = esp_timer_get_time();
      if (now_us - last_flush_us <
          static_cast<int64_t>(cfg::sd::kFlushPeriodMs) * 1000) {
        continue; // rien d'urgent : on attend le flush périodique
      }
      last_flush_us = now_us;
    }

    bool ok = true;
    size_t n = 0;
    while ((n = ring_take(chunk, sizeof(chunk))) > 0) {
      if (!write_chunk(chunk, n)) {
        ok = false;
        break;
      }
    }
    if (ok) {
      ok = (fflush(s_file) == 0) && (fsync(fileno(s_file)) == 0);
    }

    if (ok) {
      s_consecutive_errors = 0;
      s_health.store(Health::kWriting);
      continue;
    }

    // Chemin d'erreur : AUCUN ESP_LOGx tant que la carte n'est pas démontée.
    // Une ligne de log repasserait par le sink, remplirait le ring,
    // déclencherait une écriture, donc une nouvelle erreur — boucle sans fin.
    s_consecutive_errors++;
    if (s_consecutive_errors >= 3) {
      close_file();
      (void)bsp::Bsp::instance().sd_unmount();
      s_health.store(Health::kError);
      if (!s_error_reported) {
        s_error_reported = true;
        ESP_LOGE(TAG, "ecritures SD en echec, carte demontee");
      }
    }
  }
}

} // namespace

void init_ring() {
  if (s_ring) {
    return;
  }
  s_ring = static_cast<char *>(heap_caps_malloc(
      static_cast<size_t>(cfg::sd::kRingBytes), MALLOC_CAP_SPIRAM));
  if (!s_ring) {
    return; // journal SD désactivé ; UART, page Debug et logs de l'API intacts
  }
  if (!dbg_log::add_line_sink(line_sink)) {
    heap_caps_free(s_ring);
    s_ring = nullptr;
    return;
  }
  s_health.store(Health::kBuffering);
}

void start() {
  if (!s_ring) {
    return;
  }
  prefs::bump_boot_count();

  // Le sink tourne depuis init_ring(), donc avant que prefs soit chargée : les
  // lignes du début de boot sont déjà dans le ring. C'est ici qu'on tranche.
  s_enabled.store(prefs::sd_log_enabled());
  if (!s_enabled.load()) {
    ring_discard();
  }
  // La carte est montée même journal éteint : ring.wav et le mode clé USB en
  // dépendent, et l'interrupteur doit pouvoir se rallumer sans redémarrage.
  if (bsp::Bsp::instance().sd_mount() != ESP_OK) {
    s_health.store(Health::kDisabled);
    return;
  }
  // Échoue si le dossier existe déjà : ce n'est pas une erreur.
  mkdir(cfg::sd::kLogDir, 0777);

  if (xTaskCreatePinnedToCore(writer_task, "sd_log", 4096, nullptr, 1, nullptr,
                              0) != pdPASS) {
    ESP_LOGE(TAG, "creation de la tache sd_log impossible");
    s_health.store(Health::kDisabled);
  }
}

void flush_now() {
  if (!s_ring || !s_enabled.load() || s_health.load() == Health::kDisabled) {
    return;
  }
  s_flush_request.store(true);
  // La tâche tourne à 5 Hz : 500 ms couvrent deux tours. Borne dure — un
  // redémarrage ne doit jamais dépendre de la carte.
  for (int i = 0; i < 25 && ring_pending() > 0; i++) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

bool pause_and_wait(uint32_t timeout_ms) {
  s_paused.store(true);
  if (s_health.load() == Health::kDisabled) {
    return true; // aucune tâche d'écriture : rien à attendre
  }
  // La tâche tourne à 5 Hz : un acquittement demande au plus un tour, plus la
  // durée de l'écriture en cours. Borne dure — le formatage est déclenché par
  // l'utilisateur, il ne doit pas pouvoir bloquer la tâche disque de l'UI.
  const uint32_t steps = (timeout_ms + 19) / 20;
  for (uint32_t i = 0; i < steps; i++) {
    if (s_paused_ack.load()) {
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  ESP_LOGE(TAG, "pause_and_wait: la tache d'ecriture n'a pas acquitte");
  return false;
}

void resume() {
  s_paused_ack.store(false);
  s_paused.store(false);
}

void set_enabled(bool on) {
  if (s_enabled.load() == on) {
    return;
  }
  if (!on) {
    // Journalisé AVANT de couper le sink : la ligne entre dans le ring, la
    // tâche d'écriture la pose dans le fichier lors de sa vidange finale. Dans
    // l'autre ordre le fichier s'arrêterait net, sans rien qui distingue une
    // coupure volontaire d'un plantage.
    ESP_LOGI(TAG, "journal SD desactive par l'utilisateur");
    s_enabled.store(false);
    return;
  }
  s_enabled.store(true);
  // Le fichier est rouvert par la tâche au tour suivant (<= 200 ms) ; d'ici là
  // les lignes s'accumulent dans le ring, ce qui est exactement kBuffering.
  if (s_ring) {
    s_health.store(Health::kBuffering);
  }
  ESP_LOGI(TAG, "journal SD reactive par l'utilisateur");
}

// Le ring conditionne tout : sans lui aucune ligne n'est capturée, quoi que
// dise la préférence. L'UI doit montrer l'état réel, pas l'intention.
bool enabled() { return s_ring != nullptr && s_enabled.load(); }

Health health() { return s_health.load(); }

uint32_t dropped_lines() { return s_dropped.load(); }

const char *current_file() { return s_file_name; }

void self_test() {
  if (!s_enabled.load()) {
    return; // journal coupé : ne rien écrire sur la carte, pas même un test
  }
  if (bsp::Bsp::instance().sd_state() != bsp::SdState::kMounted) {
    ESP_LOGW(TAG, "self_test ignore : carte non montee");
    return;
  }
  char path[96];
  snprintf(path, sizeof(path), "%s/selftest.tmp", cfg::sd::kLogDir);
  static const char kPayload[] = "sd_log self test 0123456789\n";
  constexpr size_t kLen = sizeof(kPayload) - 1;

  FILE *f = fopen(path, "w");
  if (!f) {
    ESP_LOGE(TAG, "self_test: fopen en ecriture a echoue");
    return;
  }
  const size_t written = fwrite(kPayload, 1, kLen, f);
  fclose(f);
  if (written != kLen) {
    ESP_LOGE(TAG, "self_test: ecriture partielle (%u/%u)",
             static_cast<unsigned>(written), static_cast<unsigned>(kLen));
    unlink(path);
    return;
  }

  char back[sizeof(kPayload)] = {};
  f = fopen(path, "r");
  if (!f) {
    ESP_LOGE(TAG, "self_test: fopen en lecture a echoue");
    unlink(path);
    return;
  }
  const size_t got = fread(back, 1, sizeof(back) - 1, f);
  fclose(f);
  unlink(path);

  if (got != kLen || memcmp(back, kPayload, kLen) != 0) {
    ESP_LOGE(TAG, "self_test: relecture differente de l'ecriture");
    return;
  }
  ESP_LOGI(TAG, "self_test OK");
}

} // namespace sd_log
