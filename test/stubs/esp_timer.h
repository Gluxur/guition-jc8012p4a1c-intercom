#pragma once

// Stub host de <esp_timer.h>. Ne fournit que `esp_timer_get_time()`, la seule
// fonction du composant qu'utilisent les sources testables ici (ringtone).
//
// L'horloge est PILOTÉE PAR LE TEST, pas dérivée de l'horloge du PC : les
// garde-fous temporels (auto-désarmement de la sonnerie à 30 s) ne sont
// vérifiables que si le test peut sauter en avant. Une horloge murale rendrait
// ces cas soit intestables, soit dépendants d'un `sleep` de 30 s.
//
// Même règle que le stub esp_log.h : ne PAS l'enrichir pour faire compiler du
// code matériel. Un module qui a besoin de plus que du log et d'un compteur µs
// se teste sur cible (test_apps/).

#include <cstdint>

namespace esp_timer_stub {

inline int64_t s_now_us = 0;

inline void set_now(int64_t us) { s_now_us = us; }
inline void advance(int64_t us) { s_now_us += us; }

} // namespace esp_timer_stub

inline int64_t esp_timer_get_time() { return esp_timer_stub::s_now_us; }
