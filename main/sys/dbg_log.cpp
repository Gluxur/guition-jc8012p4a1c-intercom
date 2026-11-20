#include "dbg_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

namespace dbg_log {

namespace {

// 8 Ko en PSRAM : assez pour l'historique récent d'une négociation ICE/DTLS
// sans peser sur la SRAM interne (déjà tendue par la pile WebRTC).
constexpr size_t kRingCap = 8192;
// Ligne de log formatée max ; au-delà on tronque (débordement rare).
constexpr size_t kLineMax = 256;

char *s_ring = nullptr;   // buffer circulaire (SPIRAM)
size_t s_head = 0;        // position d'écriture
size_t s_count = 0;       // octets disponibles à la lecture (<= kRingCap)
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t s_prev_vprintf = nullptr;
bool s_inited = false;

constexpr size_t kMaxSinks = 2; // log_stream (MQTT) + sd_log (fichier)
LineSink s_sinks[kMaxSinks] = {nullptr, nullptr};

// Hook installé via esp_log_set_vprintf. Peut être appelé depuis n'importe
// quelle tâche : section critique courte sous spinlock. vsnprintf hors
// section critique (pile locale).
int hook_vprintf(const char *fmt, va_list ap) {
  char line[kLineMax];
  va_list ap_copy;
  va_copy(ap_copy, ap);
  int n = vsnprintf(line, sizeof(line), fmt, ap_copy);
  va_end(ap_copy);

  if (n > 0 && s_ring) {
    size_t len = (n < static_cast<int>(sizeof(line))) ? static_cast<size_t>(n)
                                                      : sizeof(line) - 1;
    // Deux memcpy (au plus un repli en fin de ring) au lieu d'une boucle
    // octet par octet avec un modulo par octet. Ce bloc tourne sous
    // portENTER_CRITICAL, donc INTERRUPTIONS COUPÉES : 256 divisions
    // matérielles y ajoutaient plusieurs microsecondes de latence
    // d'interruption à CHAQUE ligne de log, au détriment de l'I2S et du SDIO.
    // kLineMax (256) < kRingCap (8192) => un seul repli possible.
    portENTER_CRITICAL(&s_mux);
    size_t first = kRingCap - s_head;
    if (first > len) {
      first = len;
    }
    memcpy(s_ring + s_head, line, first);
    if (len > first) {
      memcpy(s_ring, line + first, len - first);
    }
    s_head = (s_head + len) % kRingCap;
    s_count += len;
    if (s_count > kRingCap) {
      s_count = kRingCap; // ring plein : les octets les plus anciens sont perdus
    }
    portEXIT_CRITICAL(&s_mux);

    // Tee vers les sinks secondaires (log_stream, sd_log). Hors section
    // critique : eux-mêmes prennent leurs propres verrous.
    for (size_t i = 0; i < kMaxSinks; i++) {
      if (s_sinks[i]) {
        s_sinks[i](line, len);
      }
    }
  }

  // Chaîne vers le vprintf d'origine : conserve la sortie UART inchangée.
  if (s_prev_vprintf) {
    return s_prev_vprintf(fmt, ap);
  }
  return n;
}

} // namespace

void init() {
  if (s_inited) {
    return;
  }
  s_ring = static_cast<char *>(heap_caps_malloc(kRingCap, MALLOC_CAP_SPIRAM));
  if (!s_ring) {
    // Pas de ring : on n'installe pas le hook, les logs restent sur UART.
    return;
  }
  s_prev_vprintf = esp_log_set_vprintf(hook_vprintf);
  s_inited = true;
}

size_t drain(char *dst, size_t cap) {
  if (!dst || cap == 0 || !s_ring) {
    return 0;
  }
  portENTER_CRITICAL(&s_mux);
  size_t n = (s_count < cap) ? s_count : cap;
  // Si on tronque, on garde les n octets les plus récents.
  size_t tail = (s_head + kRingCap - n) % kRingCap;
  size_t first = kRingCap - tail;
  if (first > n) {
    first = n;
  }
  memcpy(dst, s_ring + tail, first);
  if (n > first) {
    memcpy(dst + first, s_ring, n - first);
  }
  s_count = 0;
  portEXIT_CRITICAL(&s_mux);
  return n;
}

bool add_line_sink(LineSink sink) {
  if (!sink) {
    return false;
  }
  for (size_t i = 0; i < kMaxSinks; i++) {
    if (s_sinks[i] == nullptr) {
      s_sinks[i] = sink;
      return true;
    }
  }
  return false;
}

} // namespace dbg_log
