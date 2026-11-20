#include "log_stream.h"

#include <atomic>
#include <stdio.h>
#include <string.h>

#include "sys/dbg_log.h"
#include "sys/mqtt_ctrl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace log_stream {
namespace {

constexpr char TAG[] = "logstream";

// Ring propre (indépendant de celui de la page Debug). 16 Ko PSRAM : marge pour
// une fenêtre glissante de logs récents sans peser sur la SRAM interne.
constexpr size_t kRingCap = 16384;
// Taille du snapshot publié à chaque cycle (fenêtre glissante affichée par HA).
constexpr size_t kSnapMax = 2048;

char *s_ring = nullptr;
size_t s_head = 0;
size_t s_count = 0;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> s_enabled{false};
bool (*s_publish)(const char *, int) = nullptr;

// Sink appelé depuis le hook vprintf (contexte quelconque). Ultra-léger : si
// désactivé, retour immédiat (coût = un test de flag). Sinon copie sous spinlock
// court. Aucun MQTT ici.
void line_sink(const char *line, size_t len) {
  if (!s_enabled.load() || !s_ring || len == 0)
    return;
  if (len > kRingCap)
    len = kRingCap; // une ligne plus longue que le ring n'a pas de sens
  // Deux memcpy au lieu d'un modulo par octet : ce bloc coupe les interruptions
  // (portENTER_CRITICAL) et s'exécute pour chaque ligne de log quand le stream
  // HA est actif — la latence d'interruption ajoutée pénalisait l'I2S/SDIO.
  portENTER_CRITICAL(&s_mux);
  size_t first = kRingCap - s_head;
  if (first > len)
    first = len;
  memcpy(s_ring + s_head, line, first);
  if (len > first)
    memcpy(s_ring, line + first, len - first);
  s_head = (s_head + len) % kRingCap;
  s_count += len;
  if (s_count > kRingCap)
    s_count = kRingCap; // ring plein : plus ancien écrasé (fenêtre glissante)
  portEXIT_CRITICAL(&s_mux);
}

// Copie les `cap` octets les plus récents SANS les consommer : le ring reste
// une fenêtre glissante (HA affiche un historique, pas seulement le dernier
// batch). Retourne le nombre copié.
size_t peek_recent(char *dst, size_t cap) {
  portENTER_CRITICAL(&s_mux);
  size_t n = (s_count < cap) ? s_count : cap;
  size_t tail = (s_head + kRingCap - n) % kRingCap;
  size_t first = kRingCap - tail;
  if (first > n)
    first = n;
  memcpy(dst, s_ring + tail, first);
  if (n > first)
    memcpy(dst + first, s_ring, n - first);
  portEXIT_CRITICAL(&s_mux);
  return n;
}

// Échappe `src` (n octets) en chaîne JSON dans dst[cap]. Drop les codes de
// contrôle/ANSI (< 0x20 sauf \n,\t) pour garder un JSON valide et lisible.
// Retourne la longueur écrite.
int json_escape(char *dst, int cap, const char *src, size_t n) {
  int j = 0;
  for (size_t i = 0; i < n && j < cap - 2; i++) {
    char c = src[i];
    switch (c) {
    case '"':
      dst[j++] = '\\';
      dst[j++] = '"';
      break;
    case '\\':
      dst[j++] = '\\';
      dst[j++] = '\\';
      break;
    case '\n':
      dst[j++] = '\\';
      dst[j++] = 'n';
      break;
    case '\t':
      dst[j++] = '\\';
      dst[j++] = 't';
      break;
    default:
      if ((unsigned char)c >= 0x20)
        dst[j++] = c; // sinon drop (\r, ESC/ANSI, autres contrôles)
      break;
    }
  }
  return j;
}

void pub_task(void *arg) {
  (void)arg;
  // Buffers statiques (tâche unique, non réentrante) : trop gros pour la pile.
  static char snap[kSnapMax];
  static char json[kSnapMax * 2 + 64]; // pire cas : chaque octet échappé x2
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (!s_enabled.load())
      continue;
    // Déconnecté, esp_mqtt_client_publish logge "Losing qos0 data when client
    // not connected" — un avertissement qui repasse par le hook vprintf, atterrit
    // dans NOTRE ring et se fait republier au cycle suivant, générant le même
    // avertissement. Boucle de rétroaction auto-entretenue à 2 lignes/s, plus
    // 2 Ko d'UART et de carte SD par cycle pour transporter son propre bruit.
    if (!mqtt_ctrl::connected())
      continue;
    size_t n = peek_recent(snap, sizeof(snap));
    if (n == 0)
      continue;
    int j = snprintf(json, sizeof(json), "{\"log\":\"");
    j += json_escape(json + j, (int)sizeof(json) - j, snap, n);
    // ts (uptime ms) : force un état changeant pour que HA rafraîchisse.
    // snprintf renvoie la longueur voulue, pas l'écrite : borner avant de
    // publier `j` octets, sinon une troncature ferait lire hors du buffer.
    const size_t room = sizeof(json) - (size_t)j;
    int tail = snprintf(json + j, room, "\",\"ts\":%lld}",
                        (long long)(esp_timer_get_time() / 1000));
    if (tail < 0) {
      continue;
    }
    j += ((size_t)tail < room) ? tail : (int)room - 1;
    if (s_publish)
      s_publish(json, j);
  }
}

} // namespace

void init(bool (*publish)(const char *data, int len)) {
  s_publish = publish;
  s_ring = static_cast<char *>(heap_caps_malloc(kRingCap, MALLOC_CAP_SPIRAM));
  if (!s_ring)
    return; // pas de ring : le stream restera muet (logs UART/Debug intacts)
  if (!dbg_log::add_line_sink(line_sink)) {
    ESP_LOGE(TAG, "aucun slot de sink dbg_log disponible");
  }
  xTaskCreatePinnedToCore(pub_task, "logstream", 4096, nullptr, 2, nullptr, 0);
}

void set_enabled(bool en) { s_enabled.store(en); }

} // namespace log_stream
