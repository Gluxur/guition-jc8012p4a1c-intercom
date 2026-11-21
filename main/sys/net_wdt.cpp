#include "net_wdt.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/wifi_net.h"
#include "net/esphome_api.h"
#include "sys/sd_log.h"

namespace net_wdt {

namespace {

constexpr char TAG[] = "net_wdt";

// Wi-Fi muet : 5 min. C'est la panne qu'un redémarrage local peut réellement
// corriger (pile esp_hosted/SDIO coincée, association perdue sans événement).
constexpr int64_t kWifiTimeoutUs = 5LL * 60 * 1000 * 1000;
// Home Assistant absent (aucun client de l'API) alors que le Wi-Fi tient :
// 30 min et non 5. Un HA injoignable est une panne DISTANTE — redémarrer le
// boîtier n'y change rien, et un redémarrage de Home Assistant dépasse
// couramment 5 min. À 5 min, le boîtier rebootait ~5 fois pendant une panne
// d'une demi-heure, coupant chaque session en cours au passage.
constexpr int64_t kHaTimeoutUs = 30LL * 60 * 1000 * 1000;
// Un busy_probe bloqué à true (flag de session non retombé après un démontage
// raté) désarmait le chien de garde POUR TOUJOURS. Passé ce gel cumulé, on
// réarme quoi qu'en dise la sonde.
constexpr int64_t kMaxFreezeUs = 15LL * 60 * 1000 * 1000;
constexpr uint32_t kTickMs = 5000;

bool (*s_busy_probe)() = nullptr;
void (*s_on_healthy)() = nullptr;

void task(void *) {
  int64_t last_wifi_ok = esp_timer_get_time();
  int64_t last_ha_ok = last_wifi_ok;
  int64_t freeze_start = 0; // 0 = pas de gel en cours

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(kTickMs));
    const int64_t now = esp_timer_get_time();

    // Gel, et non simple saut de tick : après un appel de dix minutes le délai
    // doit repartir de zéro, pas expirer à la première mesure qui suit. Mais
    // PLAFONNÉ : un flag de session resté armé après un démontage raté aurait
    // sinon désarmé la surveillance définitivement.
    const bool busy = (s_busy_probe != nullptr && s_busy_probe());
    if (busy) {
      if (freeze_start == 0) {
        freeze_start = now;
      }
      if (now - freeze_start < kMaxFreezeUs) {
        last_wifi_ok = now;
        last_ha_ok = now;
        continue;
      }
      // Gel trop long : la sonde ment, on réarme.
      ESP_LOGW(TAG, "busy_probe bloqué depuis %lld min : surveillance réarmée",
               (long long)((now - freeze_start) / 60000000));
    } else {
      freeze_start = 0;
    }

    if (wifi_net::link_up()) {
      last_wifi_ok = now;
    }
    if (esphome_api::connected()) {
      last_ha_ok = now;
      // Point de validation "boot sain" rejoué à chaque tick tant que l'image
      // reste en PENDING_VERIFY : la fenêtre unique d'app_main ratait tout
      // Home Assistant revenu plus tard, et l'image valide finissait par être
      // rollbackée au premier reboot venu.
      if (s_on_healthy != nullptr) {
        s_on_healthy();
      }
    }

    const int64_t wifi_down_us = now - last_wifi_ok;
    const int64_t ha_down_us = now - last_ha_ok;
    if (wifi_down_us > kWifiTimeoutUs || ha_down_us > kHaTimeoutUs) {
      ESP_LOGE(TAG, "redémarrage : wifi muet %lld s, Home Assistant absent %lld s",
               (long long)(wifi_down_us / 1000000),
               (long long)(ha_down_us / 1000000));
      // Laisse l'UART écouler la ligne : sinon le diagnostic disparaît avec le
      // reset et la panne est indébuggable à distance.
      vTaskDelay(pdMS_TO_TICKS(200));
      // La cause du redémarrage vient d'être journalisée : la pousser sur la
      // carte avant de couper, sinon elle meurt dans le ring.
      sd_log::flush_now();
      esp_restart();
    }
  }
}

} // namespace

void init(bool (*busy_probe)(), void (*on_healthy)()) {
  s_busy_probe = busy_probe;
  s_on_healthy = on_healthy;
  // Priorité 1, cœur 0 : surveillance lente, elle ne doit jamais disputer le
  // cœur à la tâche de drain TCP MJPEG qui y tourne à priorité bien plus haute.
  // Pile en RAM interne, 4 Ko : la validation OTA (écriture otadata) et le hook
  // de log (tampon de 256 o + vsnprintf) passent par ici.
  if (xTaskCreatePinnedToCore(task, "net_wdt", 4096, nullptr, 1, nullptr, 0) !=
      pdPASS) {
    ESP_LOGE(TAG, "création de la tâche net_wdt impossible");
  }
}

} // namespace net_wdt
