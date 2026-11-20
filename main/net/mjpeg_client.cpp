#include "net/mjpeg_client.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <cstring>

namespace mjpeg {
namespace {

const char *TAG = "mjpeg";

// Lecture réseau par blocs. Assez grand pour limiter les appels read, assez
// petit pour rester en SRAM/PSRAM sans gaspiller.
constexpr size_t kNetChunk = 8192;

// Leviers d'expérimentation sur le coût CPU du drain (mjpeg/0=60% pour ~1 Mo/s,
// soit ~200 cycles par octet reçu — deux ordres de grandeur au-dessus d'une
// copie). Les deux candidats sont en PSRAM ; les basculer un par un en SRAM
// interne isole lequel paie.
//   kStackCaps  : toute la chaîne esp_http_client_read -> transport -> select ->
//                 lwip_recv s'exécute sur cette pile. Chaîne d'appels profonde,
//                 donc trafic de pile intense ; en PSRAM chaque push/pop est une
//                 transaction de ligne de cache.
//   kNetCaps    : destination de la copie pbuf (SRAM interne, DMA SDIO) -> tampon
//                 de lecture, faite dans le contexte de CETTE tâche (d'où son
//                 poids ici et pas dans tcpip).
// 8 Ko chacun, à prendre sur ~34 Ko de SRAM interne libre : ne PAS les passer
// tous les deux en interne sans vérifier que le DTLS du micro alloue encore
// (mbedtls veut la même SRAM).
// ⚠️ kNetCaps EN SRAM INTERNE : ESSAYÉ LE 2026-07-31, GAIN NUL, ne pas retenter.
// À ~1030 Ko/s, avant comme après : parse=110ms/s, read=885ms/s, mjpeg/0=60%.
// L'alloc avait bien lieu (SRAM libre 33-40 -> 25-32 Ko, pas de ligne de repli).
// Conclusion : dans le memcpy de parse_chunk, la SOURCE ne coûte rien — la
// lecture séquentielle de PSRAM se précharge — et tout le prix est dans
// l'écriture PSRAM du slot de mailbox, ~37 cycles/octet (≈9,6 Mo/s utiles).
// Le bus PSRAM est déjà saturé par le scan-out DSI (800x1280x2 o à ~34 Hz =
// ~70 Mo/s en lecture continue) et par le décodeur JPEG. Déplacer un tampon de
// 8 Ko n'y change rien ; seule la SUPPRESSION d'une copie le ferait.
// Hors de cause aussi : http_parser, dont le corps a un chemin rapide qui saute
// le tampon entier (http_parser.c, case s_body_identity_eof : p = data+len-1).
constexpr uint32_t kStackCaps = MALLOC_CAP_SPIRAM;
constexpr uint32_t kNetCaps = MALLOC_CAP_SPIRAM;

// Période du log d'attribution CPU. 1x/s comme le log perf de video_mjpeg :
// au-delà on tomberait dans l'interdit « pas de log dans un chemin chaud ».
constexpr int64_t kStatPeriodUs = 1000000;

struct Ctx {
  char url[256];
  Sink sink;
  std::atomic<bool> stop;
  SemaphoreHandle_t stopped;
  TaskHandle_t task;
  uint8_t *net; // buffer de lecture réseau (PSRAM)
  // Une tâche n'est pas sortie dans les temps et court toujours quelque part.
  // Collant : plus jamais de start(), sinon deux tâches partageraient s.net,
  // s.sink et s.stopped — mailbox corrompue côté consommateur et give croisé
  // qui ferait croire au stop() suivant que tout est rentré dans l'ordre.
  bool abandoned;
};

Ctx s = {};

// État de l'extracteur, porté entre les chunks : un marqueur peut tomber à
// cheval sur deux lectures réseau.
struct Extractor {
  uint8_t *buf = nullptr; // slot prêté par le sink (nullptr = pas de frame)
  size_t cap = 0;
  size_t len = 0;
  uint8_t prev = 0; // dernier octet vu (0xFF => marqueur potentiel)
};

void drop_frame(Extractor &ex) {
  if (ex.buf != nullptr) {
    s.sink.abort_frame(s.sink.ctx);
    ex.buf = nullptr;
  }
  ex.cap = 0;
  ex.len = 0;
}

// Extraction par marqueurs JPEG : SOI (FF D8) ouvre une frame, EOI (FF D9) la
// ferme. Plus robuste que parser les en-têtes multipart (boundary variable).
// memchr + memcpy au lieu d'une boucle octet par octet : à ~340 Ko/s, tester
// chaque octet individuellement coûtait un branchement et une écriture par
// octet sur la tâche qui doit drainer la socket sans relâche. Ici on ne
// s'arrête qu'aux 0xFF (≈1 octet sur 256 dans un flux JPEG entropique).
void parse_chunk(const uint8_t *data, int n, Extractor &ex) {
  int i = 0;
  while (i < n) {
    // Un 0xFF vient d'être vu (éventuellement en fin de chunk précédent) :
    // l'octet courant décide s'il s'agit d'un marqueur. Seul cas ambigu, donc
    // seul cas traité octet par octet.
    if (ex.prev == 0xFF) {
      const uint8_t b = data[i++];
      ex.prev = b;
      if (ex.buf == nullptr) {
        if (b == 0xD8) { // SOI : demander un slot au consommateur
          size_t cap = 0;
          uint8_t *buf = s.sink.begin_frame(&cap, s.sink.ctx);
          if (buf == nullptr) {
            continue; // pas de slot : resynchronisation sur le prochain SOI
          }
          if (cap < 4) {
            // Slot prêté mais inutilisable : le RENDRE. Sans abort_frame le
            // consommateur garderait son index d'écriture armé sur un slot que
            // plus personne ne remplira.
            s.sink.abort_frame(s.sink.ctx);
            continue;
          }
          ex.buf = buf;
          ex.cap = cap;
          buf[0] = 0xFF;
          buf[1] = 0xD8;
          ex.len = 2;
        }
        continue;
      }
      if (ex.len >= ex.cap) {
        drop_frame(ex);
        continue;
      }
      ex.buf[ex.len++] = b;
      if (b == 0xD9) { // EOI : frame complète
        s.sink.end_frame(ex.len, s.sink.ctx);
        ex.buf = nullptr;
        ex.cap = 0;
        ex.len = 0;
      }
      continue;
    }

    const uint8_t *ff =
        static_cast<const uint8_t *>(memchr(data + i, 0xFF, (size_t)(n - i)));
    // Bloc à traiter : jusqu'au 0xFF inclus, sinon tout le reste du chunk.
    const size_t span =
        (ff != nullptr) ? (size_t)(ff - (data + i)) + 1 : (size_t)(n - i);

    if (ex.buf != nullptr) {
      if (ex.len + span > ex.cap) {
        // Frame plus grosse que le slot : on abandonne et on resynchronise
        // sur le prochain SOI plutôt que de corrompre.
        drop_frame(ex);
      } else {
        memcpy(ex.buf + ex.len, data + i, span);
        ex.len += span;
      }
    }
    i += (int)span;
    ex.prev = data[i - 1];
  }
}

void task(void *arg) {
  (void)arg;
  Extractor ex;
  // Une seule ligne par CHANGEMENT de statut : une reconnexion toutes les 5 s
  // pendant une panne de plusieurs heures ne doit pas remplir la carte SD.
  int last_status = 200;

  while (!s.stop.load(std::memory_order_relaxed)) {
    esp_http_client_config_t cfg = {};
    cfg.url = s.url;
    // 1 s pendant l'attente des en-têtes (boucle ci-dessous), 5 s ensuite pour
    // le corps : cf. kHeaderWaitUs.
    cfg.timeout_ms = 1000;
    cfg.buffer_size = kNetChunk;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == nullptr) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "open échoué (%s) : %s", s.url, esp_err_to_name(err));
      esp_http_client_cleanup(cli);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    // go2rtc n'envoie ses en-têtes qu'avec la PREMIÈRE frame, une fois son
    // ffmpeg démarré : 2,6-3,3 s mesurés à froid. Avec un timeout unique de
    // 5 s, un démarrage un peu plus lent finissait en « statut -1 », donc en
    // backoff de 5 s, fermeture (go2rtc arrête ffmpeg) et redémarrage à froid :
    // plus de 13 s avant l'image. Attente par tranches de 1 s qui testent
    // s.stop entre chaque — un Retour pendant ce temps ne coûte plus jusqu'à
    // 5 s de join. fetch_headers est rappelable après un EAGAIN : rien n'a été
    // consommé et le parser garde son état.
    constexpr int64_t kHeaderWaitUs = 15000000;
    const int64_t t_open = esp_timer_get_time();
    int64_t hdr = esp_http_client_fetch_headers(cli);
    while (hdr == -ESP_ERR_HTTP_EAGAIN &&
           !s.stop.load(std::memory_order_relaxed) &&
           esp_timer_get_time() - t_open < kHeaderWaitUs) {
      hdr = esp_http_client_fetch_headers(cli);
    }
    esp_http_client_set_timeout_ms(cli, 5000);
    if (s.stop.load(std::memory_order_relaxed)) {
      esp_http_client_close(cli);
      esp_http_client_cleanup(cli);
      break;
    }
    const int status = esp_http_client_get_status_code(cli);
    if (hdr < 0 && status != 200) {
      // Pas de réponse du tout (go2rtc muet, lien coupé) : ce n'est pas une
      // erreur HTTP permanente, reconnexion courte plutôt que backoff de 5 s.
      if (last_status != -1) {
        last_status = -1;
        ESP_LOGW(TAG, "pas d'en-têtes de %s après %lld ms", s.url,
                 (long long)((esp_timer_get_time() - t_open) / 1000));
      }
      esp_http_client_close(cli);
      esp_http_client_cleanup(cli);
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (status != 200) {
      // esp_http_client_open() réussit pour N'IMPORTE QUEL statut : sans ce
      // test, un nom de source erroné (404) faisait scanner le corps d'erreur à
      // la recherche d'un SOI, puis reconnecter — deux fois par seconde, sans
      // une seule ligne de log. Le symptôme se présentait comme une panne
      // réseau alors que la cause est une URL.
      if (status != last_status) {
        last_status = status;
        ESP_LOGE(TAG, "HTTP %d sur %s : flux indisponible", status, s.url);
      }
      esp_http_client_close(cli);
      esp_http_client_cleanup(cli);
      // Backoff : un 4xx est permanent, inutile de marteler go2rtc. Découpé en
      // tranches car stop() n'accorde que 7 s au join : dormir 5 s d'un bloc en
      // consommerait l'essentiel.
      for (int i = 0; i < 10 && !s.stop.load(std::memory_order_relaxed); i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
      }
      continue;
    }
    last_status = 200;

    // Resync à chaque (re)connexion : on ne suppose rien du flux précédent.
    drop_frame(ex);
    ex.prev = 0;

    // Attribution du CPU de la tâche (le log `CPU:` la donne à 60% pour ~1 Mo/s
    // sans dire où). Distinction essentielle à la lecture des deux chiffres :
    //  - `parse` est du temps CPU RÉEL : parse_chunk ne bloque sur rien, son
    //    temps mur ne s'écarte de son temps CPU que d'une préemption près.
    //  - `read` est du temps MUR, attente de données comprise : il ne vaut PAS
    //    comme mesure de CPU, seulement comme borne supérieure.
    // Le verdict vient donc de `parse` seul : s'il est négligeable, le CPU part
    // dans le chemin socket (pile + copie pbuf -> tampon), pas dans l'extracteur.
    // `partiels` (lectures rendant moins que kNetChunk) dit si la couche
    // transport rend des blocs pleins ou des segments TCP : un appel par segment
    // = un select() + un verrou lwip pour 1460 octets.
    int64_t acc_read = 0, acc_parse = 0, acc_bytes = 0;
    uint32_t calls = 0, partial = 0;
    int64_t last_log = esp_timer_get_time();

    while (!s.stop.load(std::memory_order_relaxed)) {
      const int64_t t0 = esp_timer_get_time();
      int r =
          esp_http_client_read(cli, reinterpret_cast<char *>(s.net), kNetChunk);
      const int64_t t1 = esp_timer_get_time();
      if (r <= 0) {
        break; // fin de flux ou erreur -> on reconnecte
      }
      parse_chunk(s.net, r, ex);
      const int64_t t2 = esp_timer_get_time();

      acc_read += t1 - t0;
      acc_parse += t2 - t1;
      acc_bytes += r;
      calls++;
      if ((size_t)r < kNetChunk) {
        partial++;
      }

      const int64_t el = t2 - last_log;
      if (el >= kStatPeriodUs) {
        // Normalisé sur l'intervalle réel, pas sur 1 s supposée : une lecture
        // qui bloque longtemps étire la période et fausserait les ratios.
        // Unité ms/s pour être comparable au % du log `CPU:` (600 ms/s = 60%).
        ESP_LOGI(TAG,
                 "read=%lldms/s parse=%lldms/s | %lu appels/s (%lu partiels) "
                 "%lu o/appel | %luKo/s",
                 (long long)(acc_read * 1000 / el),
                 (long long)(acc_parse * 1000 / el),
                 (unsigned long)(calls * 1000000ULL / (uint64_t)el),
                 (unsigned long)(partial * 1000000ULL / (uint64_t)el),
                 (unsigned long)(calls != 0 ? acc_bytes / calls : 0),
                 (unsigned long)(acc_bytes * 1000000LL / el / 1024));
        acc_read = 0;
        acc_parse = 0;
        acc_bytes = 0;
        calls = 0;
        partial = 0;
        last_log = t2;
      }
    }

    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    if (!s.stop.load(std::memory_order_relaxed)) {
      vTaskDelay(pdMS_TO_TICKS(500)); // souffle avant reconnexion
    }
  }

  // Ne jamais laisser un slot emprunté derrière soi : le consommateur le rend.
  drop_frame(ex);
  xSemaphoreGive(s.stopped);
  vTaskDeleteWithCaps(nullptr);
}

} // namespace

bool start(const char *url, const Sink &sink) {
  if (s.abandoned) {
    ESP_LOGE(TAG, "client abandonné (tâche zombie) : redémarrage refusé");
    return false;
  }
  if (s.task != nullptr) {
    return true; // déjà démarré
  }
  if (sink.begin_frame == nullptr || sink.end_frame == nullptr ||
      sink.abort_frame == nullptr) {
    ESP_LOGE(TAG, "sink incomplet");
    return false;
  }
  s.sink = sink;
  s.stop.store(false, std::memory_order_relaxed);
  strncpy(s.url, url, sizeof(s.url) - 1);
  s.url[sizeof(s.url) - 1] = '\0';

  // Buffer réseau en PSRAM (règle projet). Le buffer d'accumulation de frame
  // appartient désormais au consommateur (mailbox du rendu).
  s.net = static_cast<uint8_t *>(heap_caps_malloc(kNetChunk, kNetCaps));
  if (s.net == nullptr && kNetCaps != MALLOC_CAP_SPIRAM) {
    // Repli PSRAM : la SRAM interne est disputée (DTLS du micro, piles). Un
    // essai de placement mémoire ne doit pas coûter la vidéo — et la ligne de
    // log évite d'attribuer à l'essai un chiffre mesuré sur le repli.
    ESP_LOGW(TAG, "SRAM interne indisponible pour le tampon réseau : repli PSRAM");
    s.net =
        static_cast<uint8_t *>(heap_caps_malloc(kNetChunk, MALLOC_CAP_SPIRAM));
  }
  if (s.net == nullptr) {
    ESP_LOGE(TAG, "alloc buffer réseau échouée");
    return false;
  }

  s.stopped = xSemaphoreCreateBinary();
  if (s.stopped == nullptr) {
    heap_caps_free(s.net);
    s.net = nullptr;
    return false;
  }

  // Pile en PSRAM comme peer_loop (SRAM interne rare). Épinglée core 0 : le
  // core 1 porte déjà peer_loop (SRTP audio, prio 5), mjpeg_rnd (PPA 22ms) et
  // aud_render (I2S, prio 6). Sans affinité, cette tâche de drain TCP flottait
  // sur core 1 et se faisait préempter -> fenêtre TCP pleine chez go2rtc -> il
  // ralentit l'envoi (livraison-bound : drop=0, recv=fps, ~2.7Mbit/s << plafond
  // WiFi = pas la bande passante mais l'ordonnancement). Core 0 est libre sous
  // MJPEG (le décodage H.264 n'y tourne pas) : le socket y est drainé en continu.
  BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
      task, "mjpeg", 8192, nullptr, 5, &s.task, 0, kStackCaps);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "création tâche échouée");
    vSemaphoreDelete(s.stopped);
    heap_caps_free(s.net);
    s.net = nullptr;
    s.stopped = nullptr;
    s.task = nullptr;
    return false;
  }
  ESP_LOGI(TAG, "client MJPEG démarré : %s", s.url);
  return true;
}

bool stop() {
  if (s.task == nullptr) {
    return true;
  }
  s.stop.store(true, std::memory_order_relaxed);
  // La tâche peut être bloquée jusqu'à timeout_ms (5 s) dans read : on attend
  // sa sortie coopérative (jamais de vTaskDelete brutal, cf. piège I2S).
  if (xSemaphoreTake(s.stopped, pdMS_TO_TICKS(7000)) != pdTRUE) {
    // Un timeout ne prouve PAS que la tâche est sortie : elle écrit encore
    // s.net dans parse_chunk et donnera s.stopped en sortant. Libérer ici,
    // c'était garantir une écriture dans un bloc rendu au tas puis un give sur
    // un sémaphore détruit.
    //
    // Nuller les handles était PIRE que fuir : la zombie fait toujours
    // xSemaphoreGive(s.stopped) en sortant (déréférencement NULL, les
    // assertions étant désactivées) et esp_http_client_read(cli, s.net, …)
    // écrirait à l'adresse 0. On garde donc tout en place et on marque le
    // client définitivement mort — start() refusera de réarmer les globaux
    // sous la tâche survivante.
    ESP_LOGE(TAG, "tâche mjpeg toujours vivante après 7 s : client abandonné "
                  "(8 Ko + 1 sémaphore fuités jusqu'au reboot)");
    s.abandoned = true;
    return false;
  }
  vSemaphoreDelete(s.stopped);
  heap_caps_free(s.net);
  s.net = nullptr;
  s.stopped = nullptr;
  s.task = nullptr;
  ESP_LOGI(TAG, "client MJPEG arrêté");
  return true;
}

} // namespace mjpeg
