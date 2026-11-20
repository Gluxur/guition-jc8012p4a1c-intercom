#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace sys {

// Garde RAII pour un mutex FreeRTOS, pendant de bsp::Bsp::LvglLock pour les
// mutex applicatifs. Les pipelines vidéo/audio prennent leur mutex sur des
// chemins à plusieurs `continue`/`return` : un give oublié ne provoque aucune
// erreur visible, il gèle définitivement le pipeline.
class MutexLock {
public:
  explicit MutexLock(SemaphoreHandle_t mux) : mux_(mux) {
    xSemaphoreTake(mux_, portMAX_DELAY);
  }
  ~MutexLock() { xSemaphoreGive(mux_); }
  MutexLock(const MutexLock &) = delete;
  MutexLock &operator=(const MutexLock &) = delete;

private:
  SemaphoreHandle_t mux_;
};

} // namespace sys
