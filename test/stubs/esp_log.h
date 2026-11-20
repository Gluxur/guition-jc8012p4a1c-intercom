#pragma once

// Stub host de <esp_log.h>. Les sources applicatives compilées dans les tests
// loggent via ESP_LOGx ; sur PC on redirige vers stderr plutôt que de tirer
// tout le composant log d'ESP-IDF (qui dépend de FreeRTOS).
// Ne PAS enrichir ce stub pour faire tourner du code matériel : si un module a
// besoin de plus que du log pour compiler sur host, c'est qu'il n'est pas
// testable ici — le tester sur cible.

#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  ESP_LOG_NONE,
  ESP_LOG_ERROR,
  ESP_LOG_WARN,
  ESP_LOG_INFO,
  ESP_LOG_DEBUG,
  ESP_LOG_VERBOSE,
} esp_log_level_t;

#ifdef __cplusplus
}
#endif

#define ESP_LOG_STUB(letter, tag, fmt, ...)                                    \
  std::fprintf(stderr, "[" #letter "] %s: " fmt "\n", tag, ##__VA_ARGS__)

#define ESP_LOGE(tag, fmt, ...) ESP_LOG_STUB(E, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) ESP_LOG_STUB(W, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) ESP_LOG_STUB(I, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ESP_LOG_STUB(D, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) ESP_LOG_STUB(V, tag, fmt, ##__VA_ARGS__)
